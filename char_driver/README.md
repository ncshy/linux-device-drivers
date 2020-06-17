# char-driver-blockingio

Code that implements a character driver with the following functionality:
Write to memory with blocking implementation
Read from memory with blocking implementation

Memory requested is only 10 bytes, mainly to quickly test the circular buffer logic.

Uses a semaphore to serialize access to the device.

Makes use of miscd(Miscellaneous) framework inorder to avoid manually creating device file nodes.
