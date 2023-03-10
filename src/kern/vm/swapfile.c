#include "swapfile.h"
#include "vm_tlb.h"
#include <uio.h>
#include <kern/stat.h>
#include <vm.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <proc.h>
#include <current.h>
#include <vmstats.h>

// S = Swapped bit (1 when not in swap file, 0 when in)
// C = Chain bit
// P = Previous bit (for double linked list)
//<----------------20------------>|<----6-----><-----6--->|
//_________________________________________________________
//|       Virtual Page Number     |     |P|C|S|     PID   |  
//|_______________________________|_______________________|
//|                         Next                          |
//|_______________________________________________________|
//|                         Prev                          |
//|_______________________________________________________|



#define IS_SWAPPED(x) ((x) & 0x00000040)
#define SET_SWAPPED(x, value) (((x) &~ 0x00000040) | (value << 6))
#define SET_PN(entry, pn) ((entry & 0x00000FFF) | (pn << 12))
#define GET_PN(entry) ((entry &~ 0x00000FFF) >> 12)
#define SET_PID(entry, pid) ((entry &~ 0x0000003F) | pid)
#define GET_PID(entry) (entry & 0x0000003F)
#if LIST_ST
//the chain is used only for the free chunk list
#define HAS_CHAIN(x) ((x) & 0x00000080)
#define HAS_PREV(x) ((x) & 0x00000100)
#define SET_CHAIN(x, value) (((x) &~ 0x00000080) | (value << 7))
#define IS_FULL(st) (st->first_free_chunk == st->last_free_chunk && !IS_SWAPPED(st->entries[st->first_free_chunk].hi))
#define SET_PREV(x, value) (((x) &~ 0x00000100) | (value << 8))
#endif

struct STE{
    uint32_t hi;
#if LIST_ST
    uint32_t next, prev;
#endif
};

struct swapTable{
    struct vnode *fp;
    struct STE *entries;
    uint32_t size;
#if LIST_ST
    uint32_t first_free_chunk;
    uint32_t last_free_chunk;
#endif
};

#if LIST_ST
static void insert_into_process_chunk_list(swap_table st, uint32_t chunk_to_add, struct proc *p){
    if(p->n_chunks == 0){
        //we need to update also the head of the chain
        p->start_st_i = chunk_to_add;
        st->entries[chunk_to_add].hi = SET_CHAIN(SET_PREV(st->entries[chunk_to_add].hi, 0), 0);
    }else{
        //the penultimate frame of the chain is updated
        //the chain is updated and the next field is updated indexing the last frame
        st->entries[p->last_st_i].hi = SET_CHAIN(st->entries[p->last_st_i].hi, 1);
        st->entries[p->last_st_i].next = chunk_to_add;
        st->entries[chunk_to_add].hi = SET_CHAIN(SET_PREV(st->entries[chunk_to_add].hi, 1), 0);
        st->entries[chunk_to_add].prev = p->last_st_i;
    }
    p->last_st_i = chunk_to_add;
    p->n_chunks++;
}
#endif

static void elf_to_swap_transfer(swap_table st, struct vnode *v, char *buffer, uint32_t len_read, uint32_t len_write, uint32_t offset, uint32_t chunk_offset, bool zero){
    struct iovec iov_swap, iov_elf;
	struct uio ku_swap, ku_elf;
    int result;
    if(zero)
        bzero((void*)buffer, len_write);
    uio_kinit(&iov_elf, &ku_elf, buffer, len_read, offset, UIO_READ);
    result = VOP_READ(v, &ku_elf);
    if(result) 
        panic("Failed loading elf into swap area!\n");

    // Write page into swapfile
    uio_kinit(&iov_swap, &ku_swap, buffer, len_write, chunk_offset, UIO_WRITE);
    result = VOP_WRITE(st->fp, &ku_swap);
    if(result) 
        panic("Failed loading elf into swap area!\n");
}

static void write_page(swap_table st, char *buffer, uint32_t len_write, uint32_t chunk_offset, bool zero){
    struct iovec iov_swap;
	struct uio ku_swap;
    int result;
    if(zero)
        bzero((void*)buffer, len_write);
    // Write page into swapfile
    uio_kinit(&iov_swap, &ku_swap, buffer, len_write, chunk_offset, UIO_WRITE);
    result = VOP_WRITE(st->fp, &ku_swap);
    if(result) 
        panic("Failed loading elf into swap area!\n");
}

swap_table swapTableInit(char swap_file_name[]){
    struct stat file_stat;
    uint32_t i;
    swap_table result = kmalloc(sizeof(*result));
    int tmp = vfs_open(swap_file_name, O_RDWR, 0, &result->fp);
    if(tmp)
        panic("VM: Failed to create Swap area\n");
    VOP_STAT(result->fp, &file_stat);
    result->size = file_stat.st_size / PAGE_SIZE;
    result->entries = (struct STE*)kmalloc(result->size * sizeof(*(result->entries)));
#if LIST_ST
    result->first_free_chunk = 0;
    for(i = 0; i < result->size - 1; i++){
         //the chain of free frames is initialized here
        result->entries[i].hi = SET_PREV(SET_SWAPPED(SET_CHAIN(result->entries[i].hi,1), 1),1);
        result->entries[i].next = i+1;
        if(i==0){
            //if i==0 set the prev bit to 0 
            result->entries[i].prev = 0;
            result->entries[i].hi = SET_PREV(result->entries[i].hi,0);
        }else{
            result->entries[i].prev=i-1;
        } 
    
    }
    
    //the last chunk has the chain bit set to 0
    result->entries[i].hi = SET_PREV(SET_SWAPPED(SET_CHAIN(result->entries[i].hi,0), 1),1);
    result->entries[i].next = 0;
    result->entries[i].prev = i-1;
    result->last_free_chunk = i;
    //print_chunks(result);

#else
    for(i = 0; i < result->size; i++){
        result->entries[i].hi = SET_SWAPPED(result->entries[i].hi,1);
    }
#endif

    return result;
}

void swapout(swap_table st, uint32_t index, paddr_t paddr, uint32_t page_number, uint32_t pid, bool invalidate){
    struct uio swap_uio;
    struct iovec iov;

    //update the first_free_chunk index
#if LIST_ST  
    delete_free_chunk(st, index);
    insert_into_process_chunk_list(st, index, curthread->t_proc);
#endif
    uio_kinit(&iov, &swap_uio, (void*)PADDR_TO_KVADDR(paddr & PAGE_FRAME), PAGE_SIZE, index*PAGE_SIZE, UIO_WRITE);
  

    // Add page into swap table
    st->entries[index].hi = SET_PN(SET_PID(SET_SWAPPED(st->entries[index].hi, 0), pid), page_number);

    
    int result = VOP_WRITE(st->fp, &swap_uio);
    if(result)     
        panic("VM_SWAP_OUT: Failed");
    if(invalidate)
        TLB_Invalidate(paddr);
}

void swapin(swap_table st, uint32_t index, paddr_t paddr){
    int result;
    struct uio swap_uio;
    struct iovec iov;

    uio_kinit(&iov, &swap_uio, (void*)PADDR_TO_KVADDR(paddr & PAGE_FRAME), PAGE_SIZE, index*PAGE_SIZE, UIO_READ);

    // Remove page from swap table
    st->entries[index].hi = SET_SWAPPED(st->entries[index].hi, 1);
   
    result=VOP_READ(st->fp, &swap_uio);
    if(result) 
        panic("VM: SWAPIN Failed");

#if LIST_ST
    //manage the free chunk list (add a free chunk)
    delete_process_chunk(st, index);
    insert_into_free_chunk_list(st, index);
#endif
}

int getFirstFreeChunckIndex(swap_table st){

#if LIST_ST
    if(!IS_FULL(st))
        return st->first_free_chunk;
    
#else
    for(uint32_t i = 0; i < st->size; i++){
        if(IS_SWAPPED(st->entries[i].hi))
            return i;
    }
    
#endif
    return -1;
    
}
 

void elf_to_swap(swap_table st, struct vnode *v, off_t offset, uint32_t init_page_n, size_t memsize, size_t filesize, pid_t PID){
    char buffer[PAGE_SIZE / 2];
    static char zero_page[PAGE_SIZE];
    int chunk_index, chunk_offset = -1;
    uint32_t n_full_chunks = filesize / PAGE_SIZE, i, j, incr = PAGE_SIZE / 2;
    uint32_t last_chunk_size = (filesize % PAGE_SIZE); // Could be 0 if the size in memory is a multiple of the page size
    uint32_t n_empty_chunks = 0;

    if(memsize - filesize > 0) {
        if(last_chunk_size + (memsize - filesize) <= PAGE_SIZE) {
            n_empty_chunks = 0;
        } else {
            n_empty_chunks = (memsize - filesize - 1 + last_chunk_size)/PAGE_SIZE;
        }
    }

    for(i = 0; i < n_full_chunks; i++, init_page_n++){
        // Get first chunck available
        chunk_index = getFirstFreeChunckIndex(st);
        if(chunk_index == -1){
            // Handle full swapfile
            panic("Out of swap space\n");
        }else{
            chunk_offset = chunk_index * PAGE_SIZE;
            for(j = 0; j < 2; j++, chunk_offset += incr, offset += incr){
                elf_to_swap_transfer(st, v, buffer, incr, incr, offset, chunk_offset, false);
            }
#if LIST_ST
            // Add page into swap table
            delete_free_chunk(st,chunk_index);
            insert_into_process_chunk_list(st, chunk_index, curthread->t_proc);
#endif
            st->entries[chunk_index].hi = SET_SWAPPED(SET_PID(SET_PN(st->entries[chunk_index].hi, init_page_n), PID), 0);
        }
    }
    if(last_chunk_size != 0) {
        chunk_index = getFirstFreeChunckIndex(st);
        if(chunk_index == -1){
            // Handle full swapfile
            panic("Out of swap space\n");
        } else {
            chunk_offset = chunk_index * PAGE_SIZE;
            if(last_chunk_size > incr) {
                elf_to_swap_transfer(st, v, buffer, incr, incr, offset, chunk_offset, false);
                offset += incr;
                chunk_offset += incr; 
                last_chunk_size -= incr;
                elf_to_swap_transfer(st, v, buffer, last_chunk_size, incr, offset, chunk_offset, true);
            } else {
                /* I need to zero-fill a portion of the page which is bigger than half-a-page */
                /* Here I'm writing the first portion of the page, completing the first half with zeros */
                elf_to_swap_transfer(st, v, buffer, last_chunk_size, incr, offset, chunk_offset, true);
                /* Then, of course, I need to zero-fill also the second portion of the page */
                chunk_offset += incr;
                write_page(st, buffer, incr, chunk_offset, true);
            }
            st->entries[chunk_index].hi = SET_SWAPPED(SET_PID(SET_PN(st->entries[chunk_index].hi, init_page_n++), PID), 0);
            add_SWAP_chunk(SWAP_0_FILLED);
        }
    }
    for(i = 0; i < n_empty_chunks; i++, init_page_n++){
        // Get first chunck available
        chunk_index = getFirstFreeChunckIndex(st);
        if(chunk_index == -1){
            // Handle full swapfile
            panic("Out of swap space\n");
        }
        chunk_offset = chunk_index * PAGE_SIZE;
        write_page(st, zero_page, PAGE_SIZE, chunk_offset, false);
#if LIST_ST
        // Add page into swap table
        delete_free_chunk(st,chunk_index);
        insert_into_process_chunk_list(st, chunk_index, curthread->t_proc);
#endif
        st->entries[chunk_index].hi = SET_SWAPPED(SET_PID(SET_PN(st->entries[chunk_index].hi, init_page_n), PID), 0);
        add_SWAP_chunk(SWAP_BLANK);
    }
    return;
}

int getSwapChunk(swap_table st, vaddr_t faultaddress, pid_t pid){
    uint32_t page_n = faultaddress >> 12, i;
#if LIST_ST
    (void)pid;
    for(i = curthread->t_proc->start_st_i; ; i = st->entries[i].next){
        if(GET_PN(st->entries[i].hi) == page_n)
            return i;
        if(!HAS_CHAIN(st->entries[i].hi))
            break;
    }
#else
    for(i = 0; i < st->size; i++){
        if(GET_PN(st->entries[i].hi) == page_n && GET_PID(st->entries[i].hi) == (uint32_t)pid && !IS_SWAPPED(st->entries[i].hi))
            return i;
    }
#endif
    return -1;
}

void all_proc_chunk_out(swap_table st){
    for(uint32_t i = 0; i < st->size; i++){
        if(GET_PID(st->entries[i].hi) == (uint32_t)curthread->t_proc->p_pid){
            st->entries[i].hi = SET_SWAPPED(st->entries[i].hi, 1);
#if LIST_ST
            delete_process_chunk(st, i);
            insert_into_free_chunk_list(st, i);
#endif
        }
    }

}

void chunks_fork(swap_table st, pid_t src_pid, pid_t dst_pid){
    uint32_t i, j;
    int free_chunk, result;
    char buffer[PAGE_SIZE / 2];
    struct uio swap_uio;
    struct iovec iov;
#if LIST_ST
    struct proc *p;
#endif
    uint32_t incr = PAGE_SIZE / 2, offset_src, offset_dst;
    for(i = 0; i < st->size; i++){
        if(GET_PID(st->entries[i].hi) == (uint32_t)src_pid && !IS_SWAPPED(st->entries[i].hi)){
            free_chunk = getFirstFreeChunckIndex(st);
            if(free_chunk == -1)
                panic("Out of swap space\n");
            offset_src = i * PAGE_SIZE;
            offset_dst = free_chunk * PAGE_SIZE;
            for(j = 0; j < 2; j++, offset_src += incr, offset_dst += incr){
                //Reading from parent process chunk
                uio_kinit(&iov, &swap_uio, buffer, incr, offset_src, UIO_READ);
                result = VOP_READ(st->fp, &swap_uio);
                if(result) 
                    panic("Failed forking chunks!\n");
                
                //Writing new chunk for child process
                uio_kinit(&iov, &swap_uio, buffer, incr, offset_dst, UIO_WRITE);
                result = VOP_WRITE(st->fp, &swap_uio);
                if(result) 
                    panic("Failed forking chunks!\n");
            }
#if LIST_ST
            delete_free_chunk(st, free_chunk);
            p = proc_search_pid(dst_pid);
            if(p != NULL)
                insert_into_process_chunk_list(st, free_chunk, p);
#endif
            st->entries[free_chunk].hi = SET_PN(SET_PID(SET_SWAPPED(st->entries[free_chunk].hi, 0), dst_pid), GET_PN(st->entries[i].hi));
        }
    }
}

void print_chunks(swap_table st){
    kprintf("\n");
    for(uint32_t i = 0; i < 10; i++){
#if LIST_ST
        kprintf("%d) : %x SWAPPED: %d  NEXT: %x PREV: %x CHAIN: %d HAS_PREV: %d\n", i, st->entries[i].hi, IS_SWAPPED(st->entries[i].hi), st->entries[i].next, st->entries[i].prev, HAS_CHAIN(st->entries[i].hi), HAS_PREV(st->entries[i].hi));
#else
        kprintf("%d) : %x SWAPPED: %d\n", i, st->entries[i].hi, IS_SWAPPED(st->entries[i].hi));
#endif
    }
#if LIST_ST
    kprintf("last) : %x SWAPPED: %d  NEXT: %x PREV: %x CHAIN: %d HAS_PREV: %d\n" , st->entries[st->last_free_chunk].hi, IS_SWAPPED(st->entries[st->last_free_chunk].hi),st->entries[st->last_free_chunk].next, st->entries[st->last_free_chunk].prev, HAS_CHAIN(st->entries[st->last_free_chunk].hi), HAS_PREV(st->entries[st->last_free_chunk].hi));
#else
    kprintf("last) : %x SWAPPED: %d" , st->entries[st->size - 1].hi, IS_SWAPPED(st->entries[st->size - 1].hi));
#endif
}

void checkDuplicatedEntries(swap_table st){
    uint32_t i, j, first_pn, first_pid, second_pn, second_pid;
    for(i = 0; i < st->size; i++){
        first_pn = GET_PN(st->entries[i].hi);
        first_pid = GET_PID(st->entries[i].hi);
        for(j = 0; j < st->size; j++){
            if(i != j){
                second_pn = GET_PN(st->entries[j].hi);
                second_pid = GET_PID(st->entries[j].hi);
                if(first_pn == second_pn && first_pid == second_pid){
                    kprintf("\nDuplicated entries!\nFirst at %d: 0x%x\nSecond at %d: 0x%x\n", i, st->entries[i].hi, j, st->entries[j].hi);
                    return;
                }
            }
        }
    }
    kprintf("\nNo duplicated entries!\n");
    return;
}

#if LIST_ST
void 
delete_free_chunk(swap_table st,uint32_t chunk_to_delete){
    if(st->first_free_chunk == chunk_to_delete){
        st->first_free_chunk = st->entries[st->first_free_chunk].next;
        // the new first_free_chunk has no previous chunk in the list 
        st->entries[st->first_free_chunk].hi = SET_PREV(st->entries[st->first_free_chunk].hi,0);
    }else{
        uint32_t prev;
        //go to the index through the free list
        //for(i = st->first_free_chunk; HAS_CHAIN(st->entries[i].hi) && st->entries[i].next != chunk_to_delete; i = st->entries[i].next);
        //get the previous chunk respect to the chunk_to_delete
        prev = st->entries[chunk_to_delete].prev;
        if(prev == st->last_free_chunk)
            panic("Maybe we forgot to add the chunk to the free list!\n");
        if(chunk_to_delete == st->last_free_chunk){
            st->last_free_chunk = prev;
            //the last has no chain
            st->entries[prev].hi = SET_CHAIN(st->entries[prev].hi,0);
        }else{
            //assign next and prev again (skipping the chunk to delete)
            st->entries[prev].next = st->entries[chunk_to_delete].next;
            st->entries[st->entries[chunk_to_delete].next].prev = st->entries[chunk_to_delete].prev;
        }
    }
    
}

void insert_into_free_chunk_list(swap_table st, uint32_t chunk_to_add){
    if(IS_FULL(st)){
        st->first_free_chunk = st->last_free_chunk = chunk_to_add;
        st->entries[chunk_to_add].hi = SET_PREV(SET_CHAIN(st->entries[chunk_to_add].hi, 0),0);
        
    }else{

        //insertion in tail
        st->entries[st->last_free_chunk].hi = SET_PREV(SET_CHAIN(st->entries[st->last_free_chunk].hi,1),1);
        st->entries[st->last_free_chunk].next = chunk_to_add;

        //the prev of the last one is the previous last one
        st->entries[chunk_to_add].prev = st->last_free_chunk; 
        //update the last_free_chunk index
        st->last_free_chunk = chunk_to_add;
        
        //the last one has no next but has a previous 
        st->entries[st->last_free_chunk].next = 0;
        st->entries[st->last_free_chunk].hi = SET_PREV(SET_CHAIN(st->entries[st->last_free_chunk].hi,0),1);
    }
}

void delete_process_chunk(swap_table st, uint32_t chunk_to_delete){
    if(curthread->t_proc->n_chunks != 1){
        if(curthread->t_proc->start_st_i == chunk_to_delete){
            curthread->t_proc->start_st_i = st->entries[chunk_to_delete].next;
            st->entries[chunk_to_delete].hi = SET_CHAIN(st->entries[chunk_to_delete].hi, 0);
            st->entries[curthread->t_proc->start_st_i].hi = SET_PREV(st->entries[curthread->t_proc->start_st_i].hi, 0);
        }else if(curthread->t_proc->last_st_i == chunk_to_delete){
            curthread->t_proc->last_st_i = st->entries[chunk_to_delete].prev;
            st->entries[curthread->t_proc->last_st_i].hi = SET_CHAIN(st->entries[curthread->t_proc->last_st_i].hi, 0);
            st->entries[chunk_to_delete].hi = SET_PREV(st->entries[chunk_to_delete].hi, 0);
        }else{
            st->entries[st->entries[chunk_to_delete].prev].next = st->entries[chunk_to_delete].next;
            st->entries[st->entries[chunk_to_delete].next].prev = st->entries[chunk_to_delete].prev;
            st->entries[chunk_to_delete].hi = SET_CHAIN(SET_PREV(st->entries[chunk_to_delete].hi, 0), 0);
        }
    }else{
        curthread->t_proc->last_st_i = curthread->t_proc->start_st_i;
        st->entries[chunk_to_delete].hi = SET_CHAIN(SET_PREV(st->entries[chunk_to_delete].hi, 0), 0);
    }
    curthread->t_proc->n_chunks--;
}

#endif