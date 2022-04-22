struct backstore_frame{
    int va;
    uint next_index;
};
struct{
    struct spinlock lock;
    struct backstore_frame backstore_bitmap[BACKSTORE_SIZE/8];
} backstore;
