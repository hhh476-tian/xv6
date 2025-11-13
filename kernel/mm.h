// Virtual Memory Area
#ifndef MM_H
#define MM_H
struct vma {
    uint64 addr; // start address of the vma
    int length; // length of the vma 
    int perm; // permissions
    struct file *f; // pointer to file
    int offset; // file offset
    int flags; // flags
};
#endif 

