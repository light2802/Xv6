#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "backstore.h"

void backstore_init(){
    initlock(&backstore.lock, "backstore");
    for(int i = 0; i <BACKSTORE_SIZE/8; i++){
	backstore.backstore_bitmap[i].va = -1;
    }
}

int store_page(struct proc *currproc, uint va){
    uint block_no;
    struct buf *frame;
    int i, j;
    int current_index;
    struct bsframe *temp = currproc->blist;
    struct bsframe *prev = temp;
    // if the blist is empty start it
    if(currproc->blist == 0){
	if((block_no = get_free_block()) == -1){
	    return -1;
	}
	currproc->blist = &(backstore.backstore_bitmap[(block_no - BACKSTORE_START) / 8]);
	acquire(&backstore.lock);
	currproc->blist->va = va;
	currproc->blist->next_index = -1;
	release(&backstore.lock);
	for(j = 0; j < 8; j++){
		frame = bget(ROOTDEV, block_no + j);
		memmove(frame->data, currproc->buf + BSIZE*j, BSIZE);
		bwrite(frame);
		brelse(frame);
	}

	    //cprintf("got index : %d\n", i);
	return 1;
    }
    // finding if the virtual address for the given process is stored on the backing store
    while(1){
	if((uint)temp->va == va){
	    current_index = ((uint)temp - (uint)(backstore.backstore_bitmap)) / sizeof(struct bsframe);
	    block_no = BACKSTORE_START + current_index * 8;
	    for(j = 0; j < 8; j++){
		frame = bget(ROOTDEV, block_no + j);
		memmove(frame->data, currproc->buf + BSIZE*j, BSIZE);
		bwrite(frame);
		brelse(frame);
	    }
	    //cprintf("got index : %d\n", i);
	    return 1;
		
	}
	if(temp->next_index == -1){
	    break;
	}
	prev = temp;
	temp = &(backstore.backstore_bitmap[temp->next_index]);
    }
    if((block_no = get_free_block()) == -1){
	return -1;
    }
    
    //cprintf("received block no : %d\n", block_no);
    acquire(&backstore.lock);
    backstore.backstore_bitmap[(block_no - BACKSTORE_START) / 8].va = va;
    backstore.backstore_bitmap[(block_no - BACKSTORE_START) / 8].next_index = -1;
    release(&backstore.lock);
    prev->next_index = (block_no - BACKSTORE_START) / 8;
    /*if(currproc->index == MAX_BACK_PAGES - 1){
	return -1;
    }*/
    //currproc->back_blocks[currproc->index++] = block_no;
    for(i = 0; i < 8; i++){
	frame = bget(ROOTDEV, block_no + i);
	memmove(frame->data, (currproc->buf) + BSIZE*i, BSIZE);
	bwrite(frame);
	brelse(frame);
    }
    return 1; 
}

uint get_free_block(){
    int i;
    for(i = 0; i < BACKSTORE_SIZE/8; i++){
	if(backstore.backstore_bitmap[i].va == -1){
	    return (BACKSTORE_START + i*8);
	}
    }
    return -1;
}

void freebs(struct proc *curproc){
    /*for(int i = 0; i < curproc->index; i++){
	backstore_bitmap[(curproc->back_blocks[i] - BACKSTORE_START) / 8] = -1;
    }    
    curproc->index = 0;*/
    struct bsframe *temp = curproc->blist;
    struct bsframe *prev =temp;
    if(temp == 0)
	return;
    acquire(&backstore.lock);
    while(temp->next_index != -1){
	temp->va = -1;
	prev = temp;
	temp = &(backstore.backstore_bitmap[temp->next_index]);
	prev->next_index = -1;
    }
    temp->va = -1;
    release(&backstore.lock);
    return;
}
