struct bsframe{
    int va;
    uint next_index;
};

struct{
    struct spinlock;
    struct bsframe backstore_bitmap[BACKSTORE_SIZE/8];
} backstore;
