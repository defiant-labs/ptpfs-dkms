#ifndef _PTPFS_FS_SB
#define _PTPFS_FS_SB


// PTP datalayer byteorder
#define PTP_DL_BE			0xF0
#define	PTP_DL_LE			0x0F




/*
 * ptpfs super-block data in memory
 */
struct ptpfs_usb_device_info
{
    /* stucture lock */
    struct semaphore sem;

    /* users using this block */
    int open_count;     

    /* endpoints */
    int inep;
    int outep;
    int intep;

    /* the usb device */
    struct usb_device *udev;
    __u8    minor;

};
struct ptpfs_sb_info
{
    /* ptp transaction ID */
    __u32 transaction_id;
    /* ptp session ID */
    __u32 session_id;

    uid_t fs_uid;
    gid_t fs_gid;

    /* data layer byteorder */
    __u8  byteorder;

    struct ptpfs_usb_device_info *usb_device;

    struct ptp_device_info *deviceinfo;

};



struct ptpfs_dirinode_fileinfo
{
    char *filename;
    int handle;
    int mode;
};
struct ptpfs_inode_data
{
    int type;
    __u32 storage;
    struct inode *parent;
    union
    {
        struct
        {
            int num_files;
            struct ptpfs_dirinode_fileinfo *file_info;
        } dircache;
    } data;
};

#define INO_TYPE_DIR        1
#define INO_TYPE_STGDIR     2

#define PTPFSSB(x) ((struct ptpfs_sb_info*)(x->u.generic_sbp))
#define PTPFSINO(x) ((struct ptpfs_inode_data*)(&x->u.generic_ip))

//free all the allocated data
extern void ptp_free_storage_info(struct ptp_storage_info* storageinfo);
extern void ptp_free_storage_ids(struct ptp_storage_ids* storageids);
extern void ptp_free_object_handles(struct ptp_object_handles *objects);
extern void ptp_free_object_info(struct ptp_object_info *object);
extern void ptp_free_data_buffer(struct ptp_data_buffer *buffer);
extern void ptp_free_device_info(struct ptp_device_info *di);

extern __u16 ptp_getdeviceinfo(struct ptpfs_sb_info *sb, struct ptp_device_info* deviceinfo);
extern int ptp_operation_issupported(struct ptpfs_sb_info *sb, __u16 operation);

// session support
extern __u16 ptp_opensession(struct ptpfs_sb_info *sb, __u32 session);
extern __u16 ptp_closesession(struct ptpfs_sb_info *sb);
// storage id support
extern __u16 ptp_getstorageids(struct ptpfs_sb_info *sb, struct ptp_storage_ids* storageids);
extern __u16 ptp_getstorageinfo(struct ptpfs_sb_info *sb, __u32 storageid, struct ptp_storage_info* storageinfo);
// object support
extern __u16 ptp_getobjecthandles (struct ptpfs_sb_info *sb, __u32 storage,
                                   __u32 objectformatcode, __u32 associationOH,
                                   struct ptp_object_handles* objecthandles);
extern __u16 ptp_getobjectinfo (struct ptpfs_sb_info *sb, __u32 handle,
                                struct ptp_object_info* objectinfo);
extern __u16 ptp_getobject (struct ptpfs_sb_info *sb, __u32 handle, struct ptp_data_buffer *data);

extern __u16 ptp_sendobjectinfo (struct ptpfs_sb_info *sb, __u32* store, 
                                 __u32* parenthandle, __u32* handle,
                                 struct ptp_object_info* objectinfo);
extern __u16 ptp_sendobject(struct ptpfs_sb_info *sb, struct ptp_data_buffer* object, __u32 size);
extern __u16 ptp_deleteobject(struct ptpfs_sb_info *sb, __u32 handle, __u32 ofc);

extern struct inode *ptpfs_get_inode(struct super_block *sb, int mode, int dev, int ino);
extern void ptpfs_set_inode_info(struct inode *ino, struct ptp_object_info *object);
extern void ptpfs_free_inode_data(struct inode *ino);

extern struct super_operations ptpfs_ops;
extern struct file_operations ptpfs_file_operations;
extern struct file_operations ptpfs_dir_operations;
extern struct address_space_operations ptpfs_fs_aops;
extern struct inode_operations ptpfs_dir_inode_operations;
extern struct file_operations ptpfs_rootdir_operations;
extern struct inode_operations ptpfs_rootdir_inode_operations;

#endif

