#include "pt.h"
#include <types.h>
#include <current.h>
#include <proc.h>
#include <vm.h>
#include <vmstats.h>

// V = validity bit
// C = chain bit (if next field has a valid value)
// K = kernel bit (if the frame has been occupied by the kernel)
//<----------------20------------>|<----6-----><----6---->|
//_________________________________________________________
//|       Virtual Page Number     |                 |K|C|V|  hi
//|_______________________________|_______________________|
//|       Next                    |           |    PID    |  low
//|_______________________________|_______________________|

#define IS_VALID(x) ((x) & 0x00000001)
#define SET_VALID(x, value) (((x) &~ 0x00000001) | value)
#define SET_PN(entry, pn) ((entry & 0x00000FFF) | (pn << 12))
#define GET_PN(entry) ((entry &~ 0x00000FFF) >> 12)
#define SET_NEXT(entry, next) ((entry & 0x00000FFF) | (next << 12))
#define GET_NEXT(entry) ((entry &~ 0x00000FFF) >> 12)
#define SET_PID(entry, pid) ((entry &~ 0x0000003F) | pid)
#define GET_PID(entry) (entry & 0x0000003F)
#define HAS_CHAIN(x) ((x) & 0x00000002)
#define SET_CHAIN(x, value) (((x) &~ 0x00000002) | (value << 1))
#define IS_KERNEL(x) ((x) & 0x00000004)
#define SET_KERNEL(x, value) (((x) &~ 0x00000004) | (value << 2))
#define IS_FULL(pt) (pt->first_free_frame == pt->last_free_frame && IS_VALID(pt->entries[pt->first_free_frame].hi))

#define FIFO_RA 1
#define RAND_RA 0
#define RA FIFO_RA

struct PTE{
    uint32_t hi, low;
};

struct pT{
    struct PTE *entries;
    uint32_t size;
    //free frames are managed as a list of pages
    //so we have a chain of free frames, first_free_frame is the index of the first free frame of the list
    //the next free frame is the one indexed by the next field in the low part
    uint32_t first_free_frame;
    uint32_t last_free_frame;
    paddr_t mem_base_addr;      /*Used to keep track of the last address occupied by the kernel before the VM system was active*/
    uint32_t *FIFO;             /*FIFO*/
    uint32_t FIFO_index_start;  /*Index used to keep track of the last inserted element*/
    uint32_t FIFO_index_last;   /*Index used to keep track of the first element which has been inserted*/
};

page_table pageTInit(uint32_t n_pages){
    uint32_t i;
    page_table tmp = kmalloc(sizeof(*tmp));
    tmp->entries = kmalloc(n_pages * sizeof(*(tmp->entries)));
#if RA
    tmp->FIFO = kmalloc(n_pages * sizeof(*(tmp->FIFO)));
    tmp->FIFO_index_start = 0;
    tmp->FIFO_index_last = n_pages - 1;
#endif
    tmp->mem_base_addr = ram_stealmem(0);
    tmp->size = n_pages;
    frame_n_k = n_pages - 1;
    tmp->first_free_frame = 0;
    for(i = 0; i < n_pages - 1; i++){
        //the chain of free frames is built here
        //when the ipt is initialized the list of free frames includes all the frames
        tmp->entries[i].hi = SET_KERNEL(SET_PN(SET_VALID(SET_CHAIN(tmp->entries[i].hi, 1), 0), 0), 0);
        tmp->entries[i].low = SET_NEXT(SET_PID(tmp->entries[i].low, 0), (i+1));
        
    }
    //the last frame has the chain bit set to 0 (no chain) 
    tmp->entries[i].hi= SET_KERNEL(SET_PN(SET_VALID(SET_CHAIN(tmp->entries[i].hi,0),0),0),0);
    tmp->entries[i].low = SET_NEXT(SET_PID(tmp->entries[i].low, 0), 0);
    tmp->last_free_frame = i;
    return tmp;
}

//Add a new entry into page table, set V, set next and chain bit to zero.
void addEntry(page_table pt, uint32_t page_n, uint32_t index, uint32_t pid){

    //Remove the page from free frames list
    if(pt->first_free_frame != pt->last_free_frame){
        if(pt->first_free_frame == index){
            pt->first_free_frame = GET_NEXT(pt->entries[pt->first_free_frame].low);
        }else{
            uint32_t i;
            for(i = pt->first_free_frame; HAS_CHAIN(pt->entries[i].hi) && GET_NEXT(pt->entries[i].low) != index; i = GET_NEXT(pt->entries[i].low));
            if(i == pt->last_free_frame)
                panic("Maybe we forgot to add the frame to the free list!\n");
            if(index == pt->last_free_frame){
                pt->last_free_frame = i;
                pt->entries[i].hi = SET_CHAIN(pt->entries[i].hi, 0);
            }else{
                pt->entries[i].low = SET_NEXT(pt->entries[i].low, GET_NEXT(pt->entries[index].low));
            }
        }
    }

    if((page_n << 12) > MIPS_KSEG0){
        //set the frame as part of the kernel
        pt->entries[index].hi = SET_PN(SET_CHAIN(SET_VALID(SET_KERNEL(pt->entries[index].hi, 1),1), 0), page_n);
        pt->entries[index].low = SET_PID(SET_NEXT(pt->entries[index].low, 0), pid);
    }else{
        //set the frame as not part of the kernel
        pt->entries[index].hi = SET_PN(SET_CHAIN(SET_VALID(SET_KERNEL(pt->entries[index].hi, 0),1), 0), page_n);
        pt->entries[index].low = SET_PID(SET_NEXT(pt->entries[index].low, 0), pid);
    }

    // Add the page into process list
    if(curthread->t_proc->n_frames == 0){
        //we need to update also the head of the chain
        curthread->t_proc->start_pt_i = index;
    }else{
        //the penultimate frame of the chain is updated
        //the chain is updated and the next field is updated indexing the last frame
        pt->entries[curthread->t_proc->last_pt_i].hi = SET_CHAIN(pt->entries[curthread->t_proc->last_pt_i].hi, 1);
        pt->entries[curthread->t_proc->last_pt_i].low = SET_NEXT(pt->entries[curthread->t_proc->last_pt_i].low, index);
    }
    curthread->t_proc->last_pt_i = index;
    curthread->t_proc->n_frames++;
    return;
}

//Return the index where page number is stored in, if page is not stored in memory, return -1
int getFrameAddress(page_table pt, uint32_t page_n, bool frame){
    //KASSERT(GET_PID(pt->entries[curthread->t_proc->start_pt_i].low) == curthread->t_proc->p_pid);
    uint32_t frame_n = -1;
    
    for(int i = curthread->t_proc->start_pt_i; i != -1; i = GET_NEXT(pt->entries[i].low)){
        if(GET_PN(pt->entries[i].hi) == page_n){
            if(frame)
                frame_n = i;
            else
                frame_n = i * PAGE_SIZE + pt->mem_base_addr;
            break;
        }
        if(!HAS_CHAIN(pt->entries[i].hi))
            break;
    }
    
    return frame_n;
}

uint32_t replace_page(page_table pt){

    uint32_t page_index;
    
#if RA
    //print_FIFO(pt);
    do{
        pt->FIFO_index_last = (pt->FIFO_index_last + 1) % pt->size;
        page_index = pt->FIFO[pt->FIFO_index_last];
    }while(IS_KERNEL(pt->entries[page_index].hi));
#else
    page_index = random() % (frame_n_k + 1);
    (void) pt;
#endif

    return page_index;
}

paddr_t pageIn(page_table pt, uint32_t pid, vaddr_t vaddr, swap_table ST) {
    paddr_t paddr;
    int chunk_index;

    paddr = insert_page(pt, vaddr, ST, -1);
    //load a frame in memory
    chunk_index = getSwapChunk(ST, vaddr, pid);
    if(chunk_index != -1){
        /* Getting the new page from the swap file */
        swapin(ST, chunk_index, paddr);
        /* statistics */ add_VM_pageFault(VM_SWAP);
    }
    /* statistics */ add_VM_pageFault(VM_ZEROED);
    return paddr;
}

void  all_proc_page_out(page_table pt){
    int i, n_frames_left, tmp;

    if(curthread->t_proc->n_frames > 1){
        for(i = curthread->t_proc->start_pt_i, n_frames_left = curthread->t_proc->n_frames; n_frames_left > 0; i = tmp, n_frames_left--){
            tmp = GET_NEXT(pt->entries[i].low);
            remove_page(pt, i);
        }
    }else{
        remove_page(pt, curthread->t_proc->start_pt_i);
    }
}


paddr_t alloc_n_contiguos_pages(uint32_t npages, page_table pt){
	uint32_t i, index;

    int free_chunk_index;

    spinlock_acquire(&k_lock);
    index = frame_n_k;

    for(i=frame_n_k; i > frame_n_k - npages; i--){
        if(IS_VALID(pt->entries[i].hi)){
            free_chunk_index= getFirstFreeChunckIndex(ST);
            if(free_chunk_index == -1){
                panic("\nOut of swap space\n");
            }
            spinlock_release(&k_lock);
            swapout(ST, free_chunk_index, (i*PAGE_SIZE) + pt->mem_base_addr, GET_PN(pt->entries[i].hi), GET_PID(pt->entries[i].low), true);
            spinlock_acquire(&k_lock);
            remove_page(pt, i);
        }
        insert_page(pt, (PADDR_TO_KVADDR((i* PAGE_SIZE) + pt->mem_base_addr)), ST, i);
    }

    frame_n_k -= npages;
    if(frame_n_k < 0)
        panic("\nPage table full of kernel pages!\n");

    if(start_index_k == -1){
        start_index_k = start_free_index;
        start_free_index = k_frames[start_free_index].next;
        k_frames[start_index_k].start_frame_n_to_remove = index;
        k_frames[start_index_k].vaddr_to_free = PADDR_TO_KVADDR(((index * PAGE_SIZE) + pt->mem_base_addr) & PAGE_FRAME);
        k_frames[start_index_k].owner = curthread->t_proc->p_pid;
        k_frames[start_index_k].n_pages = npages;
        k_frames[start_index_k].next = -1;
        k_frames[start_index_k].prev = -1;
    }else{
        int i;
        for(i = start_index_k; k_frames[i].next != -1; i = k_frames[i].next);
        k_frames[i].next = start_free_index;
        k_frames[start_free_index].prev = i;
        start_free_index = k_frames[start_free_index].next;
        k_frames[k_frames[i].next].next = -1;
        k_frames[i].start_frame_n_to_remove = index;
        k_frames[i].vaddr_to_free = PADDR_TO_KVADDR(((index * PAGE_SIZE) + pt->mem_base_addr) & PAGE_FRAME);
        k_frames[i].owner = curthread->t_proc->p_pid;
        k_frames[i].n_pages = npages;
    }
    spinlock_release(&k_lock);

    return ((index * PAGE_SIZE) + pt->mem_base_addr) & PAGE_FRAME;
	
}

paddr_t insert_page(page_table pt, vaddr_t vaddr, swap_table ST, int suggested_frame_n){

    paddr_t frame_address;
    uint32_t frame_n;

    if(suggested_frame_n == -1){
        if(IS_FULL(pt)){
            int free_chunk_index;
            frame_n = replace_page(pt);
            frame_address = frame_n * PAGE_SIZE + pt->mem_base_addr;
            free_chunk_index = getFirstFreeChunckIndex(ST);
            if(free_chunk_index == -1){
                panic("\nOut of swap space\n");
            }
            swapout(ST, free_chunk_index, frame_address, GET_PN(pt->entries[frame_n].hi), GET_PID(pt->entries[frame_n].low), true);
            /*statistics*/add_SWAP_write();
            remove_page(pt, frame_n);
        }else{
            frame_n = pt->first_free_frame;
            frame_address =  frame_n * PAGE_SIZE + pt->mem_base_addr;
        }
    }else{
        frame_n = suggested_frame_n;
        frame_address = frame_n * PAGE_SIZE + pt->mem_base_addr;
    }
    // add an entry in the first free frame of the ipt
    addEntry(pt, (vaddr & PAGE_FRAME) >> 12,  frame_n, curthread->t_proc->p_pid);
#if RA
    //Add frame into FIFO
    pt->FIFO[pt->FIFO_index_start] = frame_n;
    pt->FIFO_index_start = (pt->FIFO_index_start + 1) % pt->size;
#endif
    return frame_address;
}

void remove_page(page_table pt, uint32_t frame_n){
    struct proc *p = proc_search_pid(GET_PID(pt->entries[frame_n].low));
    // Remove the page from process list
    if(p != NULL){
        if(p->n_frames != 1){
            if(p->start_pt_i == frame_n){
                p->start_pt_i = GET_NEXT(pt->entries[frame_n].low);
            }else{
                uint32_t i;
                for(i = p->start_pt_i; HAS_CHAIN(pt->entries[i].hi) && GET_NEXT(pt->entries[i].low) != frame_n; i = GET_NEXT(pt->entries[i].low));
                if(i == p->last_pt_i)
                    panic("Are we trying to remove a frame which does not belong to the process it claims to belong to?\n");
                if(frame_n == p->last_pt_i){
                    p->last_pt_i = i;
                    pt->entries[i].hi = SET_CHAIN(pt->entries[i].hi, 0);
                }else{
                    pt->entries[i].low = SET_NEXT(pt->entries[i].low, GET_NEXT(pt->entries[frame_n].low));
                }
            }
        }else{
            p->last_pt_i = p->start_pt_i;
        }
        p->n_frames--;
    }
    // Insert the page into free list
    if(IS_FULL(pt)){
        pt->first_free_frame = pt->last_free_frame = frame_n;
    }else{
        pt->entries[pt->last_free_frame].hi = SET_CHAIN(pt->entries[pt->last_free_frame].hi, 1);
        pt->entries[pt->last_free_frame].low = SET_NEXT(pt->entries[pt->last_free_frame].low, frame_n);
        pt->last_free_frame = frame_n;
    }
    pt->entries[frame_n].hi = SET_KERNEL(SET_PN(SET_VALID(SET_CHAIN(pt->entries[frame_n].hi, 0), 0), 0), 0);
    pt->entries[frame_n].low = SET_NEXT(SET_PID(pt->entries[frame_n].low, 0), 0);
}

void pages_fork(page_table pt, uint32_t start_src_frame, pid_t dst_pid){
    uint32_t i;
    int free_chunk_index;
    uint32_t frame_address;
    for(i = start_src_frame; ; i = GET_NEXT(pt->entries[i].low)){
            free_chunk_index = getFirstFreeChunckIndex(ST);
            if(free_chunk_index == -1){
                panic("\nOut of swap space\n");
            }
            frame_address = i * PAGE_SIZE + pt->mem_base_addr;
            swapout(ST, free_chunk_index, frame_address, GET_PN(pt->entries[i].hi), dst_pid, false);
        if(!HAS_CHAIN(pt->entries[i].hi))
            break;
    }

}

void print_pt(page_table pt){
    kprintf("\n");
    for(uint32_t i = 0; i < pt->size; i++){
        kprintf("%2d) Hi: %8x low: %8x next: %2d PID: %2d PN: %8d CHAIN: %1d\n", i, pt->entries[i].hi, pt->entries[i].low, GET_NEXT(pt->entries[i].low), GET_PID(pt->entries[i].low), GET_PN(pt->entries[i].hi), HAS_CHAIN(pt->entries[i].hi));
    }
    kprintf("\nFirst free frame: %d\nLast free frame: %d\n",pt->first_free_frame, pt->last_free_frame);
    kprintf("Current process first page index: %d\nCurrent process last page index: %d\n", curthread->t_proc->start_pt_i, curthread->t_proc->last_pt_i);
}

void print_FIFO(page_table pt){
    uint32_t i;
    for(i = 0; i < pt->size; i++){
        kprintf("%2d) %6d\n", i, pt->FIFO[i]);
    }
    kprintf("FIFO_index_last: %d\n", pt->FIFO_index_last);
    kprintf("FIFO_index_start: %d\n", pt->FIFO_index_start);
}