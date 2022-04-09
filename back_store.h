struct bsframe{
    int va;
    uint next_index;
};

struct{
    struct spinlock lock;
    struct bsframe backstore_bitmap[BACKSTORE_SIZE/8];
} backstore;
