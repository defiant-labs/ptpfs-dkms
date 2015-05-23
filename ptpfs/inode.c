/*
 * ptpfs filesystem for Linux.
 *
 * This file is released under the GPL.
 */


#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/smp_lock.h>
#include <linux/usb.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <asm/uaccess.h>


#include "ptp.h"
#include "ptpfs.h"

/* Version Information */
#define DRIVER_VERSION "v0.1"
#define DRIVER_AUTHOR "Daniel Kulp <dan@kulp.com>"
#define DRIVER_DESC "USB PTP (Picture Transfer Protocol) File System Driver"

static int debug;
/* Module paramaters */
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "Debug enabled or not");

/* we can have up to this number of device plugged in at once */
#define MAX_DEVICES		8
/* array of pointers to our devices that are currently connected */
static struct ptpfs_usb_device_info  *ptp_devices[MAX_DEVICES];
/* lock to protect the minor_table structure */
static DECLARE_MUTEX (ptp_devices_mutex);


/* USB interface class */
#ifndef USB_CLASS_PTP
    #define USB_CLASS_PTP		6
#endif

/* some random number */
#define PTPFS_MAGIC	0x245234da




static inline void ptp_usb_device_delete (struct ptpfs_usb_device_info *dev)
{
    ptp_devices[dev->minor] = NULL;
    kfree(dev);
}

void ptpfs_set_inode_info(struct inode *ino, struct ptp_object_info *object)
{
    if (object->object_format!=PTP_OFC_Association && object->association_type != PTP_AT_GenericFolder)
    {
        ino->i_size = object->object_compressed_size;
        ino->i_ctime = object->capture_date;
        ino->i_mtime = ino->i_atime = object->modification_date;
    }
    else
    {
        PTPFSINO(ino)->type = INO_TYPE_DIR;
    }
    PTPFSINO(ino)->storage = object->storage_id;
}

void ptpfs_free_inode_data(struct inode *ino)
{
    int x;
    struct ptpfs_inode_data* ptpfs_data = PTPFSINO(ino);
    switch (ptpfs_data->type)
    {
    case INO_TYPE_DIR:
    case INO_TYPE_STGDIR:
        for (x = 0; x < ptpfs_data->data.dircache.num_files; x++)
        {
            kfree(ptpfs_data->data.dircache.file_info[x].filename);
        }
        kfree(ptpfs_data->data.dircache.file_info);
        ptpfs_data->data.dircache.file_info = NULL;
        ptpfs_data->data.dircache.num_files= 0;
        break;
    }
}


static void ptpfs_put_inode(struct inode *ino)
{
    //printk(KERN_INFO "%s - %d   count: %d\n",  __FUNCTION__,ino->i_ino,ino->i_count);
    ptpfs_free_inode_data(ino);
    force_delete(ino);
}

struct inode *ptpfs_get_inode(struct super_block *sb, int mode, int dev, int ino)
{
    //printk(KERN_INFO "%s -- %d\n",  __FUNCTION__, ino);
    struct inode * inode = new_inode(sb);
    if (inode)
    {
        inode->i_mode = mode;

        inode->i_uid = current->fsuid;
        inode->i_gid = current->fsgid;

        inode->i_blksize = PAGE_CACHE_SIZE;
        inode->i_blocks = 0;
        inode->i_size = 0;
        inode->i_rdev = NODEV;
        inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
        inode->i_version = 1;
        inode->i_mapping->a_ops = &ptpfs_fs_aops;
        if (ino)
        {
            inode->i_ino = ino;
        }
        memset(PTPFSINO(inode),0,sizeof(struct ptpfs_inode_data));
        insert_inode_hash(inode);
        switch (mode & S_IFMT)
        {
        case S_IFREG:
            inode->i_fop = &ptpfs_file_operations;
            break;
        case S_IFDIR:
            inode->i_op = &ptpfs_dir_inode_operations;
            inode->i_fop = &ptpfs_dir_operations;
            break;
        case S_IFLNK:
            inode->i_op = &page_symlink_inode_operations;
            break;
        default:
            init_special_inode(inode, mode, dev);
            break;
        }
    }
    return inode;
}

static void ptpfs_put_super (struct super_block * sb)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);

    ptp_free_device_info(PTPFSSB(sb)->deviceinfo);
    kfree(PTPFSSB(sb)->deviceinfo);

    PTPFSSB(sb)->usb_device->open_count--;

    if (ptp_closesession(PTPFSSB(sb))!=PTP_RC_OK)
    {
        printk(KERN_INFO "Could not close session\n");
    }
    if (PTPFSSB(sb)->usb_device->open_count == 0 &&
        PTPFSSB(sb)->usb_device->udev == NULL)
    {
        down(&ptp_devices_mutex);
        ptp_usb_device_delete(PTPFSSB(sb)->usb_device);
        up(&ptp_devices_mutex);
    }
}

static int ptpfs_statfs(struct super_block *sb, struct statfs *buf)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct ptp_storage_ids storageids;
    int x;

    buf->f_type = PTPFS_MAGIC;
    buf->f_bsize = PAGE_CACHE_SIZE;
    buf->f_namelen = 255;

    buf->f_bsize = 1024;
    buf->f_blocks = 0;
    buf->f_bfree = 0;

    //down(&params->ptp_lock);
    if (ptp_getstorageids(PTPFSSB(sb), &storageids)!=PTP_RC_OK)
    {
        printk(KERN_INFO "Error getting storage ids\n");
        storageids.n = 0;
        storageids.storage = NULL;
        buf->f_blocks = 1024;
        buf->f_bfree = 0;
        buf->f_bfree = 0;
    }

    for (x = 0; x < storageids.n; x++)
    {
        struct ptp_storage_info storageinfo;
        if ((storageids.storage[x]&0x0000ffff)==0) continue;

        memset(&storageinfo,0,sizeof(storageinfo));
        if (ptp_getstorageinfo(PTPFSSB(sb), storageids.storage[x],&storageinfo)!=PTP_RC_OK)
        {
            printk(KERN_INFO "Error getting storage info\n");
        }
        else
        {
            buf->f_blocks += storageinfo.max_capability;
            buf->f_bfree += storageinfo.free_space_in_bytes;

            ptp_free_storage_info(&storageinfo);
        }

    }
    ptp_free_storage_ids(&storageids);
    //up(&params->ptp_lock);
    buf->f_blocks /= 1024;
    buf->f_bfree /= 1024;
    buf->f_bavail = buf->f_bfree;
    return 0;
}


struct super_operations ptpfs_ops = {
    statfs:         ptpfs_statfs,
    put_super:      ptpfs_put_super,
    put_inode:      ptpfs_put_inode,
};



static int ptpfs_parse_options(char *options, uid_t *uid, gid_t *gid)
{
    char *this_char, *value, *rest;

    this_char = NULL;
    if ( options )
        this_char = strtok(options,",");
    for ( ; this_char; this_char = strtok(NULL,","))
    {
        if ((value = strchr(this_char,'=')) != NULL)
        {
            *value++ = 0;
        }
        else
        {
            printk(KERN_ERR 
                   "ptpfs: No value for mount option '%s'\n", 
                   this_char);
            return 1;
        }

        if (!strcmp(this_char,"uid"))
        {
            if (!uid)
                continue;
            *uid = simple_strtoul(value,&rest,0);
            if (*rest)
                goto bad_val;
        }
        else if (!strcmp(this_char,"gid"))
        {
            if (!gid)
                continue;
            *gid = simple_strtoul(value,&rest,0);
            if (*rest)
                goto bad_val;
        }
        else
        {
            printk(KERN_ERR "ptpfs: Bad mount option %s\n",
                   this_char);
            return 1;
        }
    }
    return 0;

    bad_val:
    printk(KERN_ERR "ptpfsf: Bad value '%s' for mount option '%s'\n", 
           value, this_char);
    return 1;
}


static struct super_block *ptpfs_read_super(struct super_block * sb, void * data, int silent)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct inode * inode;
    struct dentry * root;
    int mode   = S_IRWXUGO | S_ISVTX | S_IFDIR; 
    uid_t uid = 0;
    gid_t gid = 0;
    int x;

    if (ptpfs_parse_options (data,&uid, &gid))
        return NULL;

    set_blocksize(sb->s_dev, PAGE_CACHE_SIZE);
    sb->s_blocksize = PAGE_CACHE_SIZE;
    sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
    sb->s_magic = PTPFS_MAGIC;
    sb->s_op = &ptpfs_ops;

    PTPFSSB(sb) = kmalloc(sizeof(struct ptpfs_sb_info), GFP_KERNEL);
    memset(PTPFSSB(sb), 0, sizeof(struct ptpfs_sb_info));
    PTPFSSB(sb)->byteorder = PTP_DL_LE;
    PTPFSSB(sb)->fs_gid = gid;
    PTPFSSB(sb)->fs_uid = uid;

    down(&ptp_devices_mutex);

    for (x = 0; x < MAX_DEVICES; x++)
    {
        if (ptp_devices[x])
        {
            down(&ptp_devices[x]->sem);
            PTPFSSB(sb)->usb_device = ptp_devices[x];
            PTPFSSB(sb)->usb_device->open_count++;
            up(&ptp_devices[x]->sem);
            x = MAX_DEVICES;
        }
    }

    if (PTPFSSB(sb)->usb_device == NULL)
    {
        up(&ptp_devices_mutex);

        printk(KERN_INFO "Could not find a suitable device\n");
        kfree(PTPFSSB(sb));
        return NULL;
    }

    inode = ptpfs_get_inode(sb, S_IFDIR | mode, 0, 0);
    if (!inode) goto error;
    inode->i_op = &ptpfs_rootdir_inode_operations;
    inode->i_fop = &ptpfs_rootdir_operations;

    inode->i_uid = uid;
    inode->i_gid = gid;
    root = d_alloc_root(inode);
    if (!root)
    {
        iput(inode);
        goto error;
    }
    sb->s_root = root;

    if (ptp_opensession(PTPFSSB(sb),1)!=PTP_RC_OK)
    {
        printk(KERN_INFO "Could not open session\n");
        goto error;
    }
    PTPFSSB(sb)->deviceinfo = (struct ptp_device_info *)kmalloc(sizeof(struct ptp_device_info),GFP_KERNEL);
    if (ptp_getdeviceinfo(PTPFSSB(sb), PTPFSSB(sb)->deviceinfo)!=PTP_RC_OK)
    {
        printk(KERN_ERR "Could not get device info\nTry to reset the camera.\n");
        goto error;
    }

    up(&ptp_devices_mutex);
    return sb;

    error:
    down(&PTPFSSB(sb)->usb_device->sem);
    PTPFSSB(sb)->usb_device->open_count--;
    up(&PTPFSSB(sb)->usb_device->sem);
    up(&ptp_devices_mutex);
    return NULL;
}


static void * ptp_probe(struct usb_device *udev, unsigned int ifnum, const struct usb_device_id *id)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct ptpfs_usb_device_info *dev = NULL;
    int x;
    int i,n;
    struct usb_endpoint_descriptor *ep;

    if (udev->config->interface->altsetting->bInterfaceClass==USB_CLASS_PTP)
    {
        //PTP class
        down(&ptp_devices_mutex);

        for ( x = 0; x < MAX_DEVICES; x++)
        {
            if (ptp_devices[x] == NULL)
            {
                dev = (struct ptpfs_usb_device_info *)kmalloc(sizeof(struct ptpfs_usb_device_info), GFP_KERNEL);
                memset(dev,0,sizeof(struct ptpfs_usb_device_info));
                init_MUTEX (&dev->sem);

                dev->udev = udev;
                dev->minor = x;


                ep = udev->config->interface->altsetting->endpoint;
                n=udev->config->interface->altsetting->bNumEndpoints;
                for (i=0;i<n;i++)
                {
                    if (ep[i].bmAttributes==USB_ENDPOINT_XFER_BULK)
                    {
                        if ((ep[i].bEndpointAddress&USB_ENDPOINT_DIR_MASK)==
                            USB_ENDPOINT_DIR_MASK)
                            dev->inep=ep[i].bEndpointAddress;
                        if ((ep[i].bEndpointAddress&USB_ENDPOINT_DIR_MASK)==0)
                            dev->outep=ep[i].bEndpointAddress;
                    }
                    else if (ep[i].bmAttributes==USB_ENDPOINT_XFER_INT)
                    {
                        if ((ep[i].bEndpointAddress&USB_ENDPOINT_DIR_MASK)==
                            USB_ENDPOINT_DIR_MASK)
                        {
                            dev->intep=ep[i].bEndpointAddress;
                        }

                    }
                }

                ptp_devices[x]=dev;
                x = MAX_DEVICES;
            }
        }
        if (dev == NULL)
        {
            printk(KERN_INFO "Too many PTP devices plugged in, can not handle this device.\n");
        }

        up(&ptp_devices_mutex);
        return dev;
    }
    return NULL;
}

static void ptp_disconnect(struct usb_device *udev, void *ptr)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct ptpfs_usb_device_info *dev;
    int minor;

    dev = (struct ptpfs_usb_device_info *)ptr;

    down(&ptp_devices_mutex);
    down(&dev->sem);

    minor = dev->minor;

    /* if the device is not opened, then we clean up right now */
    if (!dev->open_count)
    {
        up(&dev->sem);
        ptp_usb_device_delete(dev);
    }
    else
    {
        dev->udev = NULL;
        up(&dev->sem);
    }
    up(&ptp_devices_mutex);
}




/* usb specific object needed to register this driver with the usb subsystem */
static struct usb_driver ptpfs_usb_driver = {
    name:       "ptpfs",
    probe:      ptp_probe,
    disconnect: ptp_disconnect,
};

static DECLARE_FSTYPE(ptpfs_fs_type, "ptpfs", ptpfs_read_super,0);

static int __init init_ptpfs_fs(void)
{
    printk(KERN_INFO DRIVER_DESC " " DRIVER_VERSION "\n");

    int result;
    /* register this driver with the USB subsystem */
    result = usb_register(&ptpfs_usb_driver);
    if (result < 0)
    {
        printk(KERN_ERR "usb_register failed for the ptpfs driver. Error number %d\n", result);
        return -1;
    }
    result = register_filesystem(&ptpfs_fs_type);
    if (result < 0)
    {
        printk(KERN_ERR "register_filesystem failed for the ptpfs driver. Error number %d\n", result);
        return -1;
    }
    return 0;
}

static void __exit exit_ptpfs_fs(void)
{
    unregister_filesystem(&ptpfs_fs_type);
    usb_deregister(&ptpfs_usb_driver);
}

module_init(init_ptpfs_fs)
module_exit(exit_ptpfs_fs)

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
