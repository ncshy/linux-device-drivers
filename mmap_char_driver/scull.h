#include <linux/ioctl.h>
#define SCULL_IOC_MAGIC 0xb2

/*Non pointer operations */
#define SCULL_IOC_QQUANTA _IO(SCULL_IOC_MAGIC, 1) //quanta is returned as return value
#define SCULL_IOC_TQUANTA _IO(SCULL_IOC_MAGIC, 2) //quanta is obtained as arg parameter
