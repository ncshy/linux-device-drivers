#include<linux/netdevice.h>
#include<linux/skbuff.h>
#include<linux/if_ether.h>
#include<linux/etherdevice.h>
#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/mm.h>
#include<linux/in6.h>
#include<linux/ip.h>

#define RX_INT_ENABLED 0x01
#define TX_INT_ENABLED 0x02

struct net_device *mydev[2];
unsigned long int timeout = 100UL;
static void snull_interrupt_hdlr(int irq, void *dev_id, struct pt_regs *regs);

/* Only shared resource in this driver, thus protected by a spinlock */
struct snull_priv {
    struct rtnl_link_stats64 stats;
    int status;  //Simulates a device status register
    struct sk_buff *skb; //Saves socket buffers
    u8 rx_int_enabled;
    u8 tx_int_enabled;
    int tx_packetlen;
    u8 *tx_packet;
    spinlock_t lock; // lock is a spinlock  used only to serialize access to snull_priv structure
};


static void status_update(struct snull_priv *priv, int intrpt_enable_flag) 
{
    if (!priv) {
        pr_warning("Could not update status, uninitialized priv");
        return;
    }
    /* can be RX_INT_ENABLED or TX_INT_ENABLED */
    priv->status |= intrpt_enable_flag ;   
}

/* Invoked when interface is brought up */
static int snull_open(struct net_device *snull_dev)
{
    if (!snull_dev)
        return -1;

    /* Copies 6 bytes, MSB is forced to even 0x00
       since MSB of multicast MAC is odd 0x01 */ 
    if (snull_dev == mydev[0]) 
        memcpy(snull_dev->dev_addr, "\0SNUL0", ETH_ALEN) ;
    else
        memcpy(snull_dev->dev_addr, "\0SNUL1", ETH_ALEN) ;
    /* Starts device 'transmission queue' which 
       is ultimately a memory that the kernel 
       assigns for the device */
    netif_start_queue(snull_dev);
    return 0;
}

/* Invoked when interface is brought down */
static int snull_stop(struct net_device *snull_dev)
{
    if(!snull_dev)
        return -1;
   
    netif_stop_queue(snull_dev);
    return 0;
}

/* Low level hw transmission interface */
static int snull_hw_tx(u8 *pkt, int len, struct net_device *snull_dev) 
{
    struct net_device *dest;
    struct snull_priv *priv;
    struct snull_priv *priv_dest;
    struct iphdr *iphdr;

    if(!pkt || !snull_dev)
        return -1;

    /* Point to start of IP Header is after Eth Header*/
    iphdr = (struct iphdr *)(pkt + ETH_HLEN);
    /* Flip the LS bit of 3rd octet of dst,src ip address */
    /* On ARM 32bit, the memory is Little Endian */
    /* To make driver architecture independent, using endian macros */
    iphdr->daddr = be32_to_cpu(cpu_to_be32(iphdr->daddr) ^ 0x00000100);
    iphdr->saddr = be32_to_cpu(cpu_to_be32(iphdr->saddr) ^ 0x00000100);
    iphdr->check = 0;
    iphdr->check = ip_fast_csum(iphdr, iphdr->ihl);  

    /* Fill in the src and dst mac addresses in pkt*/
    if (snull_dev == mydev[0]) {
        /* Etherhdr = Dest mac: src mac: protocol */
        memcpy(pkt + ETH_ALEN, snull_dev->dev_addr, ETH_ALEN);
        memcpy(pkt, mydev[1]->dev_addr, ETH_ALEN);
        dest = mydev[1];
    } else {
        memcpy(pkt + ETH_ALEN, snull_dev->dev_addr, ETH_ALEN);
        memcpy(pkt, mydev[0]->dev_addr, ETH_ALEN);
        dest = mydev[0];
    }
    /* 
       Signal that packet is ready for reception 
       Modification of snull_priv, requires the
       capture of its lock 
    */
    priv = netdev_priv(snull_dev);
    spin_lock(&priv->lock);
    priv->tx_packetlen = len;
    priv->tx_packet = pkt;
    spin_unlock(&priv->lock);  
    /* 
       Enable receive interrupt of destination
       network device 
    */
    priv_dest = netdev_priv(dest);
    spin_lock(&priv_dest->lock);
    status_update(priv_dest, RX_INT_ENABLED);
    priv_dest->rx_int_enabled = 1;
    spin_unlock(&priv_dest->lock);
    /* Invoking interrupt handler */
    snull_interrupt_hdlr(0, dest, NULL);
    return 0;
}

/* 
 The hard_start_xmit function is called
 after obtaining the dev->xmit_lock lock.
 This allows for concurrency safe usage 
 of the device. On return, it will release
 the lock and can be called immediately.
 CHECK: Where in the NW code is the lock obtained?
*/
static netdev_tx_t snull_hard_start_xmit(struct sk_buff *skb, struct net_device *snull_dev)
{
    u8 buff[ETH_ZLEN];
    u8 *data;
    struct snull_priv *priv;
    int len, ret;

    if(!skb) {
        pr_err("Socket Buffer is NULL\n");
        return -1;
    }
    
    len = skb->len;
    data = skb->data;
    /* len,data used inorder to avoid modifying skb(in case retransmission is required) */
    if (skb->len < ETH_ZLEN) {
        memset(buff, 0, ETH_ZLEN);
        memcpy(buff, skb->data, skb->len);
        len = ETH_ZLEN;
        data = buff;
    }
    /* Save the socket buffer until transmission succeeds */
    priv = netdev_priv(snull_dev);
    spin_lock(&priv->lock);
    priv->skb = skb;
    spin_unlock(&priv->lock);
    /* Record transmission start time */
    skb_tx_timestamp(skb);
    /* Call the underlying transmission mechanism */
    ret = snull_hw_tx(data, len, snull_dev);
    /* Interrupt upon successful transmission */
    snull_interrupt_hdlr(0, snull_dev, NULL);
    return NETDEV_TX_OK;
}

static void snull_tx_timeout(struct net_device *snull_dev) 
{
    struct snull_priv *priv = netdev_priv(snull_dev);
    /* Drop packets on transmission timeout */
    spin_lock(&priv->lock);
    priv->stats.tx_dropped++;
    dev_kfree_skb(priv->skb);
    spin_unlock(&priv->lock);
    return;
}

static void snull_stats_64(struct net_device *snull_dev, struct rtnl_link_stats64 *storage) 
{
    struct snull_priv *priv = netdev_priv(snull_dev);
    storage->rx_packets = priv->stats.rx_packets;
    storage->rx_dropped = priv->stats.rx_dropped;
    storage->tx_packets = priv->stats.tx_packets;
    storage->tx_dropped = priv->stats.tx_dropped;
    storage->tx_bytes = priv->stats.tx_bytes;
    storage->rx_bytes = priv->stats.rx_bytes;
}

static int snull_rx(struct net_device *snull_dev, struct snull_priv *priv, u8 *pkt, int pktlen) 
{
    struct sk_buff *skb;
    char *data;
    int rx_ret;

    if (!snull_dev || !priv) {
        pr_warn("DEVICE is NULL\n");
        return -1;
    }

    /* Request skb memory */
    skb = alloc_skb(pktlen + 2, GFP_ATOMIC);
    if (unlikely(!skb)) {
        pr_warn("Sufficient memory not available\n");
        priv->stats.rx_dropped++;
        spin_unlock(&priv->lock);
        return -1;
    }
    /* Reserve 2 bytes at head for word boundary alignment */
    skb_reserve(skb, 2);
    /* Make space for packet */
    data = skb_put(skb, pktlen);
    /* Copy packet */
    memcpy(data, pkt, pktlen);
    /* Update metadata */
    skb->dev = snull_dev;
    skb->protocol = eth_type_trans(skb, snull_dev);
    pr_info("skb->protocol is %d\n", skb->protocol);
    skb->ip_summed = CHECKSUM_UNNECESSARY;
    /* Send packet to NW stack */
    rx_ret = netif_rx(skb);
    /* Update statistics */
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += pktlen;
    return rx_ret;
}

/* Generic interrupt handler */
static void snull_interrupt_hdlr(int irq, void *dev_id, struct pt_regs *regs)
{
    struct net_device *snull_dev = (struct net_device *)dev_id;
    struct snull_priv *priv = netdev_priv(snull_dev);
    struct net_device *dest;
    struct snull_priv *priv_dest;
    //int ret = 0; /* How do interrupt handlers handle return errors? */
    int status;
    u8 *pkt = NULL;
    int pktlen;

    dest = ((snull_dev == mydev[0]) ? mydev[1]:mydev[0]);
    priv_dest = netdev_priv(dest);
    spin_lock(&priv->lock);
    /* Obtain packet from HW */
    status = priv->status;
    pr_info("netdev:%x status register value is %d \n", snull_dev, status);
    priv->status = 0;
    if (priv->rx_int_enabled && (status & RX_INT_ENABLED)) {
        pkt = priv_dest->tx_packet;
        pktlen = priv_dest->tx_packetlen;
        /* Function that deals with the receive internals */
        pr_info("Invoking snull_rx\n");
        snull_rx(snull_dev, priv, pkt, pktlen);     
        /* Signal to sender device that transmission was a success */
        spin_lock(&priv_dest->lock);
        status_update(priv_dest, TX_INT_ENABLED);
        priv_dest->tx_int_enabled = 1;
        spin_unlock(&priv_dest->lock);
        priv->rx_int_enabled = 0;
        pkt = NULL;
    } else if (priv->tx_int_enabled && (status & TX_INT_ENABLED)) {
        pr_info("Inside tx_hdlr  \n");
        /* Update transmission stats and free skb */
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        //pkt = priv->tx_packet;
        dev_kfree_skb_irq(priv->skb);
        priv->tx_int_enabled = 0;
    }
    spin_unlock(&priv->lock);
    //if(pkt)
      //  kfree(pkt);
}

struct net_device_ops snull_ops = {
    .ndo_open = snull_open,
    .ndo_stop = snull_stop,
    .ndo_start_xmit = snull_hard_start_xmit,
    .ndo_tx_timeout = snull_tx_timeout,
    .ndo_get_stats64 = snull_stats_64,
};

/* Runtime initialization */
static void dev_bringup(struct net_device *dev) 
{
    struct snull_priv *priv;
    /* Sets up broad ethernet layer default settings */
    ether_setup(dev); 
    /* Custom device settings */
    dev->netdev_ops = &snull_ops;
    dev->watchdog_timeo = timeout;
    dev->flags |= IFF_NOARP;
    dev->features |= NETIF_F_HW_CSUM;
    /* NETIF_F_NO_CSUM not supported in v5.4.70 
       dev->hard_header_cache not supported
    dev->features |= NETIF_F_NO_CSUM;
    dev->hard_header_cache = NULL;
    */
    /* Initialize device private memory */
    priv = netdev_priv(dev);
    memset(priv, 0, sizeof(struct snull_priv));
    spin_lock_init(&priv->lock);
}

/*  
    Initializes the driver and supported devices.
    This part registers the interface associated
    with a device. Interfaces are a mechanism by
    which userspace can use the underlying device.
*/
static int __init init_nw(void)
{
    int i;
    /* Initialize the devices the driver handles */
    mydev[0] = alloc_netdev(sizeof(struct snull_priv), "ldd%d", NET_NAME_UNKNOWN, dev_bringup);
    pr_info("Init mydev[0]\n");
    mydev[1] = alloc_netdev(sizeof(struct snull_priv), "ldd%d", NET_NAME_UNKNOWN, dev_bringup);
    pr_info("Init mydev[1]\n");
    if (!mydev[0] || !mydev[1]) {
        pr_alert("Failed to allocate resources for device \n");
        goto err;
    }
    /* Once registered, kernel can immediately use device */
    for (i = 0; i < 2; i++) {
        int result = register_netdev(mydev[i]);
        if (result < 0) {
            pr_alert("Failed to register device\n");
            goto err;
        }
    }
    return 0;
    /* Reclaim resources */
err:
    for (i = 0; i < 2; i++)
        if (mydev[i]) {
            unregister_netdev(mydev[i]);
            free_netdev(mydev[i]);
        }
    return -1;
}

/*
    Frees the device resources
*/
static void __exit exit_nw(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (mydev[i]) {
            unregister_netdev(mydev[i]);
            free_netdev(mydev[i]);
            pr_info("Unregister mydev[%d]\n", i);
        }
}

module_init(init_nw);
module_exit(exit_nw);
MODULE_LICENSE("GPL");











