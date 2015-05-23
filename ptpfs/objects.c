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
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/ctype.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#include "ptp.h"              
#include "ptpfs.h"



static int ptpfs_get_dir_data(struct inode *inode)
{
    int x;
    struct ptpfs_inode_data* ptpfs_data = PTPFSINO(inode);
    if (ptpfs_data->data.dircache.file_info == NULL)
    {
        struct ptp_object_handles objects;
        objects.n = 0;
        objects.handles = NULL;

        ptpfs_free_inode_data(inode);
        ptpfs_data->data.dircache.num_files = 0;
        ptpfs_data->data.dircache.file_info = NULL;

        if (ptpfs_data->type == INO_TYPE_DIR)
        {
            if (ptp_getobjecthandles(PTPFSSB(inode->i_sb), ptpfs_data->storage, 0x000000, inode->i_ino,&objects) != PTP_RC_OK)
            {
                return 0;
            }
        }
        else
        {
            if (ptp_getobjecthandles(PTPFSSB(inode->i_sb), inode->i_ino, 0x000000, 0xffffffff ,&objects) != PTP_RC_OK)
            {
                return 0;
            }
        }

        int size = objects.n*sizeof(struct ptpfs_dirinode_fileinfo);
        struct ptpfs_dirinode_fileinfo* finfo = (struct ptpfs_dirinode_fileinfo*)kmalloc(size, GFP_KERNEL);
        ptpfs_data->data.dircache.file_info = finfo;
        if (finfo == NULL)
        {
            return 0;
        }
        memset(ptpfs_data->data.dircache.file_info,0,size);
        for (x = 0; x < objects.n; x++)
        {
            struct ptp_object_info object;
            memset(&object,0,sizeof(object));
            if (ptp_getobjectinfo(PTPFSSB(inode->i_sb),objects.handles[x],&object)!=PTP_RC_OK)
            {
                ptpfs_free_inode_data(inode);
                ptp_free_object_handles(&objects);
                return 0;
            }

            if (ptpfs_data->type == INO_TYPE_STGDIR && 
                (object.storage_id != inode->i_ino || object.parent_object != 0))
            {
                continue;
            }
            int mode = DT_REG;
            if (object.object_format==PTP_OFC_Association && object.association_type == PTP_AT_GenericFolder)
                mode = DT_DIR;

            finfo[ptpfs_data->data.dircache.num_files].filename = object.filename;
            finfo[ptpfs_data->data.dircache.num_files].handle = objects.handles[x];
            finfo[ptpfs_data->data.dircache.num_files].mode = mode;
            ptpfs_data->data.dircache.num_files++;
            object.filename = NULL;

            ptp_free_object_info(&object);
        }
        ptp_free_object_handles(&objects);
    }
    return 1;
}


static int ptpfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct inode *inode = filp->f_dentry->d_inode;
    struct dentry *dentry = filp->f_dentry;
    struct ptpfs_inode_data* ptpfs_data = PTPFSINO(inode);
    int offset = filp->f_pos;
    int x,ino;

    if (filp->f_version == inode->i_version && offset)
    {

        return filp->f_pos;
    }
    filp->f_version = inode->i_version;

    switch (offset)
    {
    case 0:
        if (filldir(dirent, ".", 1, offset, inode->i_ino, DT_DIR) < 0)
        {
            break;
        }
        offset++;
        filp->f_pos++;
        /* fall through */
    case 1:
        spin_lock(&dcache_lock);
        ino = dentry->d_parent->d_inode->i_ino;
        spin_unlock(&dcache_lock);
        if (filldir(dirent, "..", 2, offset, ino, DT_DIR) < 0)
            break;
        filp->f_pos++;
        offset++;
    default:
        if (!ptpfs_get_dir_data(inode))
        {
            return filp->f_pos;
        }

        filp->f_pos=2;
        for (x = 0; x < ptpfs_data->data.dircache.num_files; x++)
        {
            if (filldir(dirent, 
                        ptpfs_data->data.dircache.file_info[x].filename, 
                        strlen(ptpfs_data->data.dircache.file_info[x].filename), 
                        filp->f_pos, 
                        ptpfs_data->data.dircache.file_info[x].handle ,
                        ptpfs_data->data.dircache.file_info[x].mode) < 0)
            {
                ptpfs_free_inode_data(inode);
                return filp->f_pos;
            }

            filp->f_pos++;
        }
        return filp->f_pos;
    }
    //down(&params->ptp_lock);

    //up(&params->ptp_lock);
    return filp->f_pos;
}

static struct dentry * ptpfs_lookup(struct inode *dir, struct dentry *dentry)
{
    int x;
    //printk(KERN_INFO "%s\n",  __FUNCTION__);

    struct ptpfs_inode_data* ptpfs_data = PTPFSINO(dir);
    if (!ptpfs_get_dir_data(dir))
    {
        d_add(dentry, NULL);
        return NULL;
    }
    if (ptpfs_data->type != INO_TYPE_DIR  && ptpfs_data->type != INO_TYPE_STGDIR)
    {
        d_add(dentry, NULL);
        return NULL;
    }
    for (x = 0; x < ptpfs_data->data.dircache.num_files; x++)
    {

        if (!memcmp(ptpfs_data->data.dircache.file_info[x].filename,dentry->d_name.name,dentry->d_name.len))
        {
            struct ptp_object_info object;
            if (ptp_getobjectinfo(PTPFSSB(dir->i_sb),ptpfs_data->data.dircache.file_info[x].handle,&object)!=PTP_RC_OK)
            {
                d_add(dentry, NULL);
                return NULL;
            }
            int mode = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;
            if (object.object_format==PTP_OFC_Association && object.association_type == PTP_AT_GenericFolder)
            {
                mode |= S_IFDIR; 
            }
            else
            {
                mode |= S_IFREG;
            }
            struct inode *newi = ptpfs_get_inode(dir->i_sb, mode  , 0,ptpfs_data->data.dircache.file_info[x].handle);
            PTPFSINO(newi)->parent = dir;
            ptpfs_set_inode_info(newi,&object);
            //atomic_inc(&newi->i_count);    /* New dentry reference */
            d_add(dentry, newi);
            ptp_free_object_info(&object);
            return NULL;
        }
    }

    d_add(dentry, NULL);
    return NULL;
}

static int ptpfs_file_readpage(struct file *filp, struct page *page)
{
    //printk(KERN_INFO "%s\n",  __FUNCTION__);
    struct inode *inode;
    int ret;
    struct ptp_data_buffer *data=(struct ptp_data_buffer*)filp->private_data;

    inode = page->mapping->host;

    if (!PageLocked(page))
        PAGE_BUG(page);

    ret = -ESTALE;

    /* work out how much to get and from where */
    int offset = page->index << PAGE_CACHE_SHIFT;
    int size   = min((size_t)(inode->i_size - offset),(size_t)PAGE_SIZE);
    char *buffer = kmap(page);
    clear_page(buffer);

    /* read the contents of the file from the server into the page */
    int block = 0;
    while (block < data->num_blocks && offset > data->blocks[block].block_size)
    {
        offset -= data->blocks[block].block_size;
        block++;
    }

    if (block == data->num_blocks)
    {
        kunmap(page);
        ret = -ESTALE;
        goto error;
    }

    int toCopy = min(size,data->blocks[block].block_size-offset);
    memcpy(buffer,&data->blocks[block].block[offset],toCopy);
    size -= toCopy;
    int pos = toCopy;
    block++;
    while (size && block < data->num_blocks)
    {
        toCopy = min(size,data->blocks[block].block_size);
        memcpy(&buffer[pos],data->blocks[block].block,toCopy);
        size -= toCopy;
        pos += toCopy;
        block++;
    }

    if (block == data->num_blocks && size > 0)
    {
        kunmap(page);
        ret = -ESTALE;
        goto error;
    }


    kunmap(page);
    flush_dcache_page(page);
    SetPageUptodate(page);
    unlock_page(page);

    return 0;

    error:
    SetPageError(page);
    unlock_page(page);
    return ret;

} 


static ssize_t ptpfs_write(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
    //printk(KERN_INFO "%s   pos: %d   count:%d\n",  __FUNCTION__,(int)*ppos,count);
    struct ptp_data_buffer *data=(struct ptp_data_buffer*)filp->private_data;

    struct ptp_block *blocks = kmalloc(sizeof(struct ptp_block)*(data->num_blocks+1), GFP_KERNEL);
    if (data->blocks) memcpy(blocks,data->blocks,sizeof(struct ptp_block)*data->num_blocks);
    blocks[data->num_blocks].block_size=count;
    blocks[data->num_blocks].block = kmalloc(count, GFP_KERNEL);
    memcpy(blocks[data->num_blocks].block,buf,count);
    if (data->blocks) kfree(data->blocks);
    data->blocks = blocks;
    data->num_blocks++;
    *ppos += count;
    
    return count;
}


static int ptpfs_open(struct inode *ino, struct file *filp)
{
    //printk(KERN_INFO "%s   flags: %X\n",  __FUNCTION__, filp->f_flags);
    struct ptp_data_buffer *data=(struct ptp_data_buffer*)kmalloc(sizeof(struct ptp_data_buffer), GFP_KERNEL);
    int retval;
    memset(data,0,sizeof(struct ptp_data_buffer));


    switch (filp->f_flags & O_ACCMODE)
    {
    case O_RDWR:
        return -EFAULT;
    case O_RDONLY:
        retval = ptp_getobject(PTPFSSB(ino->i_sb),ino->i_ino,data);
        if (retval == PTP_RC_OK)
        {
            filp->private_data = data;
            return 0;
        }
        break;
    case O_WRONLY:
        filp->private_data = data;
        return 0;
    }

    kfree(data);
    return -EFAULT;
}
static int ptpfs_release(struct inode *ino, struct file *filp)
{
    //printk(KERN_INFO "%s    object:%X    dcount: %d\n",  __FUNCTION__,ino->i_ino, filp->f_dentry->d_count);
    struct ptp_data_buffer *data=(struct ptp_data_buffer*)filp->private_data;
    if (data)
    {
        switch (filp->f_flags & O_ACCMODE)
        {
        case O_RDWR:
        case O_WRONLY:
            {
                struct ptp_object_info object;
                memset(&object,0,sizeof(object));
                if (ptp_getobjectinfo(PTPFSSB(ino->i_sb),ino->i_ino,&object)!=PTP_RC_OK)
                {
                    ptp_free_data_buffer(data);
                    kfree(data);
                    return -EPERM;
                }

                ptp_deleteobject(PTPFSSB(ino->i_sb),ino->i_ino,0);
                int count = 0;
                int x;
                for (x = 0; x < data->num_blocks; x++)
                {
                    count += data->blocks[x].block_size;
                }


                int storage = object.storage_id;
                int parent = object.parent_object;
                int handle = ino->i_ino;
                object.object_compressed_size=count;
                int ret = ptp_sendobjectinfo(PTPFSSB(ino->i_sb), &storage, &parent, &handle, &object);
                ret = ptp_sendobject(PTPFSSB(ino->i_sb),data,count);

                ino->i_size = count;
                ino->i_mtime = CURRENT_TIME;
                ptpfs_free_inode_data(ino);//uncache
                ptpfs_free_inode_data(PTPFSINO(ino) -> parent);//uncache

                //the inode number will change.  Kill this inode so next dir lookup will create a new one
                dput(filp->f_dentry);
                //iput(ino);
            }
            break;
        }

        ptp_free_data_buffer(data);
        kfree(data);
    }
    return 0;
}



struct mime_map
{
    char *ext;
    int type;
};                    
const struct mime_map mime_map[] = {
    {"txt",PTP_OFC_Text},
    {"mp3",PTP_OFC_MP3},
    {"mpg",PTP_OFC_MPEG},
    {"wav",PTP_OFC_WAV},
    {"avi",PTP_OFC_AVI},
    {"asf",PTP_OFC_ASF},
    {"jpg",PTP_OFC_EXIF_JPEG},
    {"tif",PTP_OFC_TIFF},
    {"bmp",PTP_OFC_BMP},
    {"gif",PTP_OFC_GIF},
    {"pcd",PTP_OFC_PCD},
    {"pct",PTP_OFC_PICT},
    {"png",PTP_OFC_PNG},
    {0,0},
};




static inline int get_format(unsigned char *filename, int l)
{
    char buf[4];
    int x;
    while (l)
    {
        l--;
        if (filename[l] == '.')
        {
            filename = &filename[l];
            l = 0;
            if (strlen(filename) > 4 || strlen(filename) == 1)
            {
                return PTP_OFC_Undefined;
            }
            filename++;
            for (x = 0; x < strlen(filename); x++)
            {
                buf[x] = __tolower(filename[x]);
            }
            buf[strlen(filename)] = 0;
            const struct mime_map *map = mime_map;
            while (map->ext)
            {
                if (!strcmp(buf,map->ext))
                {
                    return map->type;
                }
                map++;
            }
        }
    }
    return PTP_OFC_Undefined;
}

static int ptpfs_create(struct inode *dir,struct dentry *d,int i) 
{
    //printk(KERN_INFO "%s   %s %d\n",__FUNCTION__,d->d_name.name,i);
    __u32 storage;
    __u32 parent;
    __u32 handle;
    struct ptp_object_info objectinfo;
    memset(&objectinfo,0,sizeof(objectinfo));

    storage = PTPFSINO(dir)->storage;
    if (PTPFSINO(dir)->type == INO_TYPE_STGDIR)
    {
        parent = 0xffffffff;
    }
    else
    {
        parent = dir->i_ino;
    }

    objectinfo.filename = (unsigned char *)d->d_name.name;
    objectinfo.object_format=get_format((unsigned char *)d->d_name.name,d->d_name.len);
    objectinfo.object_compressed_size=0;

    int ret = ptp_sendobjectinfo(PTPFSSB(dir->i_sb), &storage, &parent, &handle, &objectinfo);

    if (ret == PTP_RC_OK)
    {
        struct ptp_data_buffer data;
        struct ptp_block block;
        unsigned char buf[10];

        memset(&data,0,sizeof(data));
        memset(&block,0,sizeof(block));
        data.blocks=&block;
        data.num_blocks = 1;
        block.block_size = 0;
        block.block = buf;

        ret = ptp_sendobject(PTPFSSB(dir->i_sb),&data,0);


        if (handle == 0)
        {
            //problem - it didn't return the right handle - need to do something find it
            struct ptp_object_handles objects;
            objects.n = 0;
            objects.handles = NULL;

            ptpfs_get_dir_data(dir);
            if (PTPFSINO(dir)->type == INO_TYPE_DIR)
            {
                ptp_getobjecthandles(PTPFSSB(dir->i_sb), PTPFSINO(dir)->storage, 0x000000, dir->i_ino,&objects);
            }
            else
            {
                ptp_getobjecthandles(PTPFSSB(dir->i_sb), dir->i_ino, 0x000000, 0xffffffff ,&objects);
            }

            int x,y;
            for (x = 0; x < objects.n && !handle; x++ )
            {
                for (y = 0; y < PTPFSINO(dir)->data.dircache.num_files; y++)
                {
                    if (PTPFSINO(dir)->data.dircache.file_info[y].handle == objects.handles[x])
                    {
                        objects.handles[x] = 0;
                        y = PTPFSINO(dir)->data.dircache.num_files;
                    }
                }
                if (objects.handles[x])
                {
                    handle = objects.handles[x]; 
                }
            }
            ptp_free_object_handles(&objects);
        }


        int mode = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_IFREG;
        struct inode *newi = ptpfs_get_inode(dir->i_sb, mode  , 0,handle);
        PTPFSINO(newi)->parent = dir;
        ptpfs_set_inode_info(newi,&objectinfo);
        atomic_inc(&newi->i_count);    /* New dentry reference */
        d_instantiate(d, newi);

        objectinfo.filename = NULL;
        ptp_free_object_info(&objectinfo);

        ptpfs_free_inode_data(dir);//uncache
        dir->i_version++;
        newi->i_mtime = CURRENT_TIME;
        return 0;
    }
    objectinfo.filename = NULL;
    ptp_free_object_info(&objectinfo);
    return -EPERM;


}

static int ptpfs_mkdir(struct inode *ino,struct dentry *d,int i)
{
    //printk(KERN_INFO "%s   %s %d\n",__FUNCTION__,d->d_name.name,i);
    __u32 storage;
    __u32 parent;
    __u32 handle;
    struct ptp_object_info objectinfo;
    memset(&objectinfo,0,sizeof(objectinfo));

    storage = PTPFSINO(ino)->storage;
    if (PTPFSINO(ino)->type == INO_TYPE_STGDIR)
    {
        parent = 0xffffffff;
    }
    else
    {
        parent = ino->i_ino;
    }

    objectinfo.filename = (unsigned char *)d->d_name.name;
    objectinfo.object_format=PTP_OFC_Association;
    objectinfo.association_type = PTP_AT_GenericFolder;

    int ret = ptp_sendobjectinfo(PTPFSSB(ino->i_sb), &storage, &parent, &handle, &objectinfo);
    /*
    if (ret == PTP_RC_OK)
    {

        struct ptp_data_buffer data;
        memset(&data,0,sizeof(data));
        ret = ptp_sendobject(PTPFSSB(ino->i_sb),&data,0);
    }
    */


    objectinfo.filename = NULL;
    ptp_free_object_info(&objectinfo);

    if (ret == PTP_RC_OK)
    {
        ptpfs_free_inode_data(ino);//uncache
        ino->i_version++;

        return 0;
    }
    return -EPERM;
}

int ptpfs_unlink(struct inode *dir,struct dentry *d)
{
    //printk(KERN_INFO "%s   %s\n",__FUNCTION__,d->d_name.name);
    int x;

    struct ptpfs_inode_data* ptpfs_data = PTPFSINO(dir);
    if (!ptpfs_get_dir_data(dir))
    {
        return -EPERM;
    }
    if (ptpfs_data->type != INO_TYPE_DIR  && ptpfs_data->type != INO_TYPE_STGDIR)
    {
        return -EPERM;
    }


    for (x = 0; x < ptpfs_data->data.dircache.num_files; x++)
    {

        if (!memcmp(ptpfs_data->data.dircache.file_info[x].filename,d->d_name.name,d->d_name.len))
        {
            int ret = ptp_deleteobject(PTPFSSB(dir->i_sb),ptpfs_data->data.dircache.file_info[x].handle,0);
            if (ret == PTP_RC_OK)
            {
                ptpfs_free_inode_data(dir);//uncache
                dir->i_version++;

                return 0;

            }
        }
    }
    return -EPERM;
}


static int ptpfs_rmdir(struct inode *ino ,struct dentry *d)
{
    return ptpfs_unlink(ino,d);
}

struct address_space_operations ptpfs_fs_aops = {
    readpage:   ptpfs_file_readpage,
};
struct file_operations ptpfs_dir_operations = {
    read:       generic_read_dir,
    readdir:    ptpfs_readdir,
};
struct file_operations ptpfs_file_operations = {
    read:       generic_file_read, 
    write:      ptpfs_write,
    open:       ptpfs_open,
    release:    ptpfs_release,
};
struct inode_operations ptpfs_dir_inode_operations = {
    lookup:     ptpfs_lookup,
    mkdir:      ptpfs_mkdir,
    rmdir:      ptpfs_rmdir,
    unlink:     ptpfs_unlink,

    create:     ptpfs_create,
};

/*
struct file_operations ptpfs_file_operations = {
    write:      ptpfs_file_write,
    mmap:       generic_file_mmap,
    fsync:      ptpfs_sync_file,
};
struct inode_operations ptpfs_dir_inode_operations = {
    create:     ptpfs_create,
    rename:     ptpfs_rename,
};
*/
