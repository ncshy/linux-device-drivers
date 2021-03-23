# mmap_char_driver

Character driver that registers a device to the kernel using standard `cdev`.  
Supports `non-blocking IO`.  
On every write syscall invocation, device memory is cleared, freed and reallocated.  

# Implementing mmap:  
```
mmap code figures out the physical memory of the 1st block in the 1st quanta, it then gets the page frame number by shifting right with PAGE_SHIFT.
The page frame number is then used to build a page table entry with the new vm_area of the mmap calling process. 

If defined with MAP_PRIVATE, then user changes are not flushed to the physical memory.  
If defined with MAP_SHARED, the user change is reflected.  
```
Normally mmap() is used for mapping file blocks or peripheral registers and video buffers into userspace memory.  

mmap() is also used instead of brk() with MAP_ANONYMOUS settings by malloc. I think its used for dynamic memory requests of 128KB and larger.

# Implementing ioctl:
ioctl macros are defined in scull.h  
The `SCULL_MAGIC` was obtained from an unused MAGIC number.  
```
scull_ioctl.c file contains userspace support for accessing the ioctl commands and for manipulating the mmap'ed memory buffer.  

```


