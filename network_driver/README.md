Network Driver implementation based on Linux Device Drivers 3rd Edition.

Installation:
The driver(ldd\_nw.ko) is compiled for Kernel 5.4.70 running on a BCM2837 SoC(Rpi 3).
As a result, it had to be cross-compiled for the ARM architecture. 

Working:

The Network device driver, tells the kernel to associate 2 device interfaces(ldd0 and ldd1) that the driver will control.

The devices are then initialized with specific attributes, before they are registered with the kernel. At this point, the kernel can use these devices. This isn't a joke, immediately I saw ICMPV6 Router Solicitation packets being sent over the interface.
The devices consider themselves as Ethernet devices, thus use a ether\_setup() helper to bootstrap some of the device attributes. Further options were set manually including the operations to support.

The basic implementation supports, opening the device(when the interface is brought up), sending, receiving and display statistics. All these functions are registered in 'net\_device\_ops' struct.

How the example devices differ from actual devices:

An actual device would be a Network Interface Card(NIC). For a NIC, the driver would have to engage in additional steps, such as read/write of NIC registers, figuring out IRQ values associated with the device, interfacing with the bus(PCIe, USB). 
Since the example devices are entirely virtual(no hardware backing it), the registers and IRQs are simulated. There exists a 32bit status register, with the 1st bit for rx\_interrupts and 2nd bit for tx\_interrupts. The IRQ is given a value of 0 and does not matter because it is not invoked by the HW device, but rather from the driver function itself. In effect, it is a synchronous invocation of the interrupt handler. 

Finally, if interface ldd0 has an IP of 192.168.2.2, then interface ldd1 is given an IP of 192.168.3.1 (the least significant bit of the 3rd octet has to be flipped). The 4th octet for ldd1 can be any byte value except 0,255 and 2.  
When ldd0 sends a packet to 192.168.2.1, inside the transmit function, the 3rd octet lsb is flipped for both source IP address and dest IP address. 
As a result, the packet becomes of the form src:dst = 192.168.3.2: 192.168.3.1 . This modified packet is stored in system memory and then the interrupt handler is invoked for the receiving device. 
The receiving device as part of its interrupt handling reads the packet from system memory and sends it up to the kernel for processing. As can be seen, the modified packet simulates an incoming packet to the receiving device.  
Each device has a private memory associated with it, this was crucial to simulate status registers, store modified packets, maintain transmission statistics etc. The private memory is 'struct snull\_priv'. It also has a spinlock embedded within it to serialize access to this structure. 

Interesting findings from running the driver:

1) tcpdump and other packet sniffers, obtain their packet from the wire. But what is the wire? It sounds like the physical medium, but in reality the receive a copy of a packet at the boundary point of the Kernel Network stack and the device driver. 
This was shown to be the case, since the driver mangles the SRC,DST ip addresses of the packets(as mentioned earlier), yet the tcpdump for the transmitting interface showed the pre-mangled IP values. 
However on the receiving side, tcpdump showed the mangled IP addresses, thus confirming the sniffing happened by attaching a hook to the kernel network stack. 

2) The importance of IP checksumming. At first, I thought it was not important, since on the receive side, I was setting the field:
skb-\>ip\_summed = CHECKSUM\_UNNECESSARY; 

 However this plays an important role none the less since on the transmit side, I had to set:
 dev-\>features |= NETIF\_F\_HW\_CSUM;
 Even though there is no hardware checksumming(since there is no hardware), without this option, the ICMP echo request was not getting back an ICMP echo reply. ether\_setup does not fill in this value, so it has to be manually set to NETIF\_F\_HW\_CSUM. 
