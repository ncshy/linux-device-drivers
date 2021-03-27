# char-driver-blockingio

Code that implements a character driver with the following functionality:

```
Write to memory with blocking implementation if buffer is full

Read from memory with blocking implementation if buffer is empty 

Ensure that write pointer and read pointers are tracked and updated properly for the circular buffer. 

Memory requested is only `10 bytes`, mainly to quickly test the circular buffer logic.  

Uses a semaphore to serialize access to the device.  

Makes use of `miscd(Miscellaneous) framework` inorder to avoid manually creating device file nodes.  

```

Write can be tested using:  
```
echo "world" > /dev/chsleep1  
```

Read can be tested with:  
```
dd if=/dev/chsleep1 count=n bs=1  
'n' represents n bytes to be read.
```
