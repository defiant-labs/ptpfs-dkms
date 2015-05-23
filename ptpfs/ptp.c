#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>

/* Define generic byte swapping functions */
#include <asm/byteorder.h>
#include <asm/unaligned.h>

#include "ptp.h"
#include "ptpfs.h"
#include "ptp-pack.h"

static int ptp_io_read(struct ptpfs_sb_info *sb, unsigned char *bytes, unsigned int size)
{
    int retval = 0;
    int count = 0;


    memset(bytes,0,size);
    /* do an immediate bulk read to get data from the device */
    int pipe =  usb_rcvbulkpipe (sb->usb_device->udev, sb->usb_device->inep);
    //retval = usb_ptp_bulk_msg (sb,pipe,bytes, size,&count);
    retval = usb_bulk_msg ( sb->usb_device->udev,pipe,bytes, size,&count,3*HZ);

    /* if the read was successful, copy the data to userspace */
    if (!retval)
    {
        retval = count;
    }
    else if (retval == -EPIPE)
    {
        //stall
        usb_clear_halt(sb->usb_device->udev,pipe);
    }
    return retval;
}
static int ptp_io_write(struct ptpfs_sb_info *sb, unsigned char *bytes, unsigned int size)
{
    ssize_t bytes_written = 0;
    int retval = 0;

    /* verify that the device wasn't unplugged */
    if (sb->usb_device->udev == NULL)
    {
        retval = -ENODEV;
        goto exit;
    }

    /* verify that we actually have some data to write */
    if (size == 0)
    {
        goto exit;
    }


    int pipe =  usb_sndbulkpipe (sb->usb_device->udev, sb->usb_device->outep);
    //retval = usb_ptp_bulk_msg(sb,pipe,bytes, size,&bytes_written);
    retval = usb_bulk_msg( sb->usb_device->udev,pipe,bytes, size,&bytes_written,5*HZ);

    if (retval == -EPIPE)
    {
        //stall
        usb_clear_halt(sb->usb_device->udev,pipe);
    }
    if (!retval)
    {
        retval = bytes_written;
    }

    exit:

    return retval;
}


static __u16 ptp_usb_sendreq(struct ptpfs_sb_info *sb, struct ptp_container* req)
{
    //printk(KERN_INFO "%s\n",__FUNCTION__);
    int ret;
    struct ptp_usb_bulkcontainer usbreq;

    /* build appropriate USB container */
    usbreq.length=htod32p(sb,PTP_USB_BULK_REQ_LEN-(sizeof(__u32)*(5-req->nparam)));
    usbreq.type=htod16p(sb,PTP_USB_CONTAINER_COMMAND);
    usbreq.code=htod16p(sb,req->code);
    usbreq.trans_id=htod32p(sb,req->transactionID);
    usbreq.payload.params.param1=htod32p(sb,req->param1);
    usbreq.payload.params.param2=htod32p(sb,req->param2);
    usbreq.payload.params.param3=htod32p(sb,req->param3);
    usbreq.payload.params.param4=htod32p(sb,req->param4);
    usbreq.payload.params.param5=htod32p(sb,req->param5);
    /* send it to responder */
    ret=ptp_io_write(sb,(unsigned char *)&usbreq,PTP_USB_BULK_REQ_LEN-(sizeof(__u32)*(5-req->nparam)));

    if (ret < 0 )
    {
        ret = PTP_ERROR_IO;
    }
    return PTP_RC_OK;
}

static __u16 ptp_usb_senddata(struct ptpfs_sb_info *sb, struct ptp_container* ptp, struct ptp_data_buffer *data, unsigned int size)
{
    //printk(KERN_INFO "%s\n",__FUNCTION__);
    int ret,block,bytesCopied,bytesToCopy,offset;

    unsigned char buf[512];
    struct ptp_usb_bulkcontainer *usbdata;
    usbdata = (struct ptp_usb_bulkcontainer*)buf;


    /* build appropriate USB container */
    usbdata->length=htod32p(sb,PTP_USB_BULK_HDR_LEN+size);
    usbdata->type=htod16p(sb,PTP_USB_CONTAINER_DATA);
    usbdata->code=htod16p(sb,ptp->code);
    usbdata->trans_id=htod32p(sb,ptp->transactionID);
    if (size < (512-PTP_USB_BULK_HDR_LEN))
    {
        memcpy(&buf[PTP_USB_BULK_HDR_LEN],data->blocks[0].block,size);
        ret=ptp_io_write(sb,buf, size+PTP_USB_BULK_HDR_LEN);
    }
    else
    {
        bytesCopied = (512-PTP_USB_BULK_HDR_LEN);
        memcpy(&buf[PTP_USB_BULK_HDR_LEN],data->blocks[0].block,bytesCopied);
        ret=ptp_io_write(sb,buf, 512);
        offset = bytesCopied;
        block = 0;
        while (bytesCopied < size)
        {
            bytesToCopy = data->blocks[block].block_size-offset;
            if (bytesToCopy > 512) bytesToCopy = 512;
            memcpy(buf,&data->blocks[block].block[offset],bytesToCopy);
            offset += bytesToCopy;
            if (offset >= data->blocks[block].block_size)
            {
                block++;
                offset = 0;
            }
            if (bytesToCopy < 512 && (bytesToCopy+bytesCopied) < size)
            {
                int toCopy = 512-bytesToCopy;
                if (toCopy > data->blocks[block].block_size) toCopy = data->blocks[block].block_size;
                memcpy(&buf[bytesToCopy],data->blocks[block].block,toCopy);
                offset = toCopy;
                bytesToCopy += toCopy;
            }
            ret=ptp_io_write(sb,buf,bytesToCopy);
            if (ret < 0)
            {
                return PTP_ERROR_IO;
            }
            bytesCopied += bytesToCopy;
        }
    }
    if (ret < 0)
    {
        return PTP_ERROR_IO;
    }
    return PTP_RC_OK;
}

#define MAX_SEG_SIZE (4096*4)
static __u16 ptp_usb_getdata(struct ptpfs_sb_info *sb, struct ptp_container* ptp, struct ptp_data_buffer *data)
{
    //printk(KERN_INFO "%s\n",__FUNCTION__);
    int ret;
    int x;
    unsigned int len;
    struct ptp_usb_bulkcontainer usbdata;
    memset(&usbdata,0,sizeof(usbdata));

    if (data->blocks != NULL)
    {
        return PTP_ERROR_BADPARAM;
    }
    /* read first(?) part of data */

    ret=ptp_io_read(sb,(unsigned char *)&usbdata,sizeof(usbdata));
    if (ret < 0)
    {
        return PTP_ERROR_IO;
    }
    else if (dtoh16p(sb,usbdata.type)!=PTP_USB_CONTAINER_DATA)
    {
        return PTP_ERROR_DATA_EXPECTED;
    }
    else if (dtoh16p(sb,usbdata.code)!=ptp->code)
    {
        return dtoh16p(sb,usbdata.code);
    }
    /* evaluate data length */
    len=dtoh32p(sb,usbdata.length)-PTP_USB_BULK_HDR_LEN;

    int num_seg = 1;
    if (len > PTP_USB_BULK_PAYLOAD_LEN)
    {
        num_seg = (len-PTP_USB_BULK_PAYLOAD_LEN)/MAX_SEG_SIZE+2;
    }
    /* allocate memory for data */
    data->blocks =(struct ptp_block*)kmalloc(num_seg*sizeof(struct ptp_block),GFP_KERNEL);
    memset(data->blocks,0,num_seg*sizeof(struct ptp_block));
    //first block is what we already got
    data->num_blocks = 1;
    data->blocks[0].block_size = len<PTP_USB_BULK_PAYLOAD_LEN?len:PTP_USB_BULK_PAYLOAD_LEN;
    data->blocks[0].block = kmalloc(data->blocks[0].block_size,GFP_KERNEL);
    memcpy(data->blocks[0].block,usbdata.payload.data,data->blocks[0].block_size);
    for (x = 1; x < num_seg; x++ )
    {
        data->blocks[x].block = kmalloc(MAX_SEG_SIZE,GFP_KERNEL);
        data->blocks[x].block_size = MAX_SEG_SIZE;
        memset(data->blocks[x].block,0,MAX_SEG_SIZE);
        if (data->blocks[x].block == NULL)
        {
            ptp_free_data_buffer(data);
            return PTP_ERROR_IO;
        }
        ret=ptp_io_read(sb,data->blocks[x].block,MAX_SEG_SIZE);
        if (ret < 0)
        {
            ptp_free_data_buffer(data);
            return PTP_ERROR_IO;
        }

        data->num_blocks++;
    }
    return PTP_RC_OK;
}

__u16 ptp_usb_getresp(struct ptpfs_sb_info *sb, struct ptp_container* resp)
{
    //printk(KERN_INFO "%s\n",__FUNCTION__);
    int ret;
    struct ptp_usb_bulkcontainer usbresp;
    memset(&usbresp,0,sizeof(usbresp));

    /* read response, it should never be longer than sizeof(usbresp) */
    ret=ptp_io_read(sb,(unsigned char *)&usbresp, sizeof(usbresp));

    if (ret < 0)
    {
        return PTP_ERROR_IO;
    }
    else if (dtoh16p(sb,usbresp.type)!=PTP_USB_CONTAINER_RESPONSE)
    {
        return PTP_ERROR_RESP_EXPECTED;
    }
    else if (dtoh16p(sb,usbresp.code)!=resp->code)
    {
        return dtoh16p(sb,usbresp.code);
    }
    /* build an appropriate PTPContainer */
    resp->code=dtoh16p(sb,usbresp.code);
    resp->sessionID=sb->session_id;
    resp->transactionID=dtoh32p(sb,usbresp.trans_id);
    resp->param1=dtoh32p(sb,usbresp.payload.params.param1);
    resp->param2=dtoh32p(sb,usbresp.payload.params.param2);
    resp->param3=dtoh32p(sb,usbresp.payload.params.param3);
    resp->param4=dtoh32p(sb,usbresp.payload.params.param4);
    resp->param5=dtoh32p(sb,usbresp.payload.params.param5);
    return PTP_RC_OK;
}

// major PTP functions

// Transaction data phase description
#define PTP_DP_NODATA		0x0000	// No Data Phase
#define PTP_DP_SENDDATA		0x0001	// sending data
#define PTP_DP_GETDATA		0x0002	// geting data
#define PTP_DP_DATA_MASK	0x00ff	// data phase mask

// Number of PTP Request phase parameters
#define PTP_RQ_PARAM0		0x0000	// zero parameters
#define PTP_RQ_PARAM1		0x0100	// one parameter
#define PTP_RQ_PARAM2		0x0200	// two parameters
#define PTP_RQ_PARAM3		0x0300	// three parameters
#define PTP_RQ_PARAM4		0x0400	// four parameters
#define PTP_RQ_PARAM5		0x0500	// five parameters

/**
 * ptp_transaction:
 * params:	PTPParams*
 * 		PTPContainer* ptp	- general ptp container
 * 		__u16 flags		- lower 8 bits - data phase description
 * 		unsigned int sendlen	- senddata phase data length
 * 		char** data		- send or receive data buffer pointer
 *
 * Performs PTP transaction. ptp is a PTPContainer with appropriate fields
 * filled in (i.e. operation code and parameters). It's up to caller to do
 * so.
 * The flags decide thether the transaction has a data phase and what is its
 * direction (send or receive). 
 * If transaction is sending data the sendlen should contain its length in
 * bytes, otherwise it's ignored.
 * The data should contain an address of a pointer to data going to be sent
 * or is filled with such a pointer address if data are received depending
 * od dataphase direction (send or received) or is beeing ignored (no
 * dataphase).
 * The memory for a pointer should be preserved by the caller, if data are
 * beeing retreived the appropriate amount of memory is beeing allocated
 * (the caller should handle that!).
 *
 * Return values: Some PTP_RC_* code.
 * Upon success PTPContainer* ptp contains PTP Response Phase container with
 * all fields filled in.
 **/
static __u16 ptp_transaction (struct ptpfs_sb_info *sb, struct ptp_container* ptp, __u16 flags, unsigned int sendlen, struct ptp_data_buffer *data)
{
    if ((sb==NULL) || (ptp==NULL))
    {
        return PTP_ERROR_BADPARAM;
    }

    /* lock this object */
    down(&sb->usb_device->sem);
    /* verify that the device wasn't unplugged */
    if (sb->usb_device->udev == NULL)
    {
        up (&sb->usb_device->sem);
        return PTP_ERROR_BADPARAM;
    }


    __u16 result = PTP_RC_OK;
    ptp->transactionID=sb->transaction_id++;
    ptp->sessionID=sb->session_id;
    // send request
    result = ptp_usb_sendreq(sb, ptp);
    if (result != PTP_RC_OK)
    {
        goto done;
    }
    // is there a dataphase?
    switch (flags&PTP_DP_DATA_MASK)
    {
    case PTP_DP_SENDDATA:
        result = ptp_usb_senddata(sb, ptp, data, sendlen);
        break;
    case PTP_DP_GETDATA:
        result = ptp_usb_getdata(sb, ptp, data);
        break;
    case PTP_DP_NODATA:
        break;
    default:
        result = PTP_ERROR_BADPARAM;
        goto done;
    }
    if (result != PTP_RC_OK)
    {
        goto done;
    }
    result = ptp_usb_getresp(sb, ptp);

    done:    /* unlock the device */
    up (&sb->usb_device->sem);
    return result;
}

void ptp_free_device_info(struct ptp_device_info *di)
{
    if (di->vendor_extension_desc) kfree(di->vendor_extension_desc);
    if (di->manufacturer) kfree(di->manufacturer);
    if (di->model) kfree(di->model);
    if (di->device_version) kfree(di->device_version);
    if (di->serial_number) kfree(di->serial_number);

}


__u16 ptp_getdeviceinfo(struct ptpfs_sb_info *sb, struct ptp_device_info* deviceinfo)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));

    ptp.code=PTP_OC_GetDeviceInfo;
    ptp.nparam=0;
    ret=ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, &data);
    if (ret == PTP_RC_OK) ptp_unpack_DI(sb, &data, deviceinfo);
    ptp_free_data_buffer(&data);
    return ret;
}



/* Non PTP protocol functions */
/* devinfo testing functions */

int ptp_operation_issupported(struct ptpfs_sb_info *sb, __u16 operation)
{
    int i=0;

    for (;i<sb->deviceinfo->operations_supported_len;i++)
    {
        if (sb->deviceinfo->operations_supported[i]==operation)
            return 1;
    }
    return 0;
}


int ptp_property_issupported(struct ptpfs_sb_info *sb, __u16 property)
{
    int i=0;

    for (;i<sb->deviceinfo->device_properties_supported_len;i++)
    {
        if (sb->deviceinfo->device_properties_supported[i]==property)
            return 1;
    }
    return 0;
}

__u16 ptp_opensession(struct ptpfs_sb_info *sb, __u32 session) 
{
    __u16 ret;
    struct ptp_container ptp;
    memset(&ptp,0,sizeof(ptp));

    /* SessonID field of the operation dataset should always be set to 0 for OpenSession request! */
    sb->session_id=0x00000000;
    /* TransactionID should be set to 0 also! */
    sb->transaction_id=0x0000000;

    ptp.code=PTP_OC_OpenSession;
    ptp.param1=session;
    ptp.nparam=1;

    ret=ptp_transaction(sb, &ptp, PTP_DP_NODATA, 0, NULL);
    /* now set the global session id to current session number */
    sb->session_id=session;
    return ret;
}

__u16 ptp_closesession(struct ptpfs_sb_info *sb)
{
    struct ptp_container ptp;
    memset(&ptp,0,sizeof(ptp));

    ptp.code=PTP_OC_CloseSession;
    ptp.nparam=0;
    return ptp_transaction(sb, &ptp, PTP_DP_NODATA, 0, NULL);
}


void ptp_free_storage_ids(struct ptp_storage_ids* storageids) 
{
    if (storageids->storage) kfree(storageids->storage);
}

__u16 ptp_getstorageids(struct ptpfs_sb_info *sb, struct ptp_storage_ids* storageids)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));
    ptp.code=PTP_OC_GetStorageIDs;
    ptp.nparam=0;
    ret=ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, &data);
    if (ret == PTP_RC_OK) ptp_unpack_SIDs(sb, &data, storageids);
    ptp_free_data_buffer(&data);
    return ret;
}

void ptp_free_storage_info(struct ptp_storage_info* storageinfo)
{
    if (storageinfo->storage_description) kfree(storageinfo->storage_description);
    if (storageinfo->volume_label) kfree(storageinfo->volume_label);
}

__u16 ptp_getstorageinfo(struct ptpfs_sb_info *sb, __u32 storageid, struct ptp_storage_info* storageinfo)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));
    ptp.code=PTP_OC_GetStorageInfo;
    ptp.param1=storageid;
    ptp.nparam=1;
    ret=ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, &data);
    if (ret == PTP_RC_OK) ptp_unpack_SI(sb, &data, storageinfo);
    ptp_free_data_buffer(&data);
    return ret;
}



void ptp_free_object_handles(struct ptp_object_handles *objects)
{
    if (objects->handles) kfree(objects->handles);
}
void ptp_free_object_info(struct ptp_object_info *object)
{
    if (object->filename) kfree(object->filename);
    if (object->keywords) kfree(object->keywords);
}

__u16 ptp_getobjecthandles (struct ptpfs_sb_info *sb, __u32 storage,
                            __u32 objectformatcode, __u32 associationOH,
                            struct ptp_object_handles* objecthandles)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));

    ptp.code=PTP_OC_GetObjectHandles;
    ptp.param1=storage;
    ptp.param2=objectformatcode;
    ptp.param3=associationOH;
    ptp.nparam=3;
    ret=ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, &data);
    if (ret == PTP_RC_OK) ptp_unpack_OH(sb, &data, objecthandles);
    ptp_free_data_buffer(&data);
    return ret;
}

__u16 ptp_getobjectinfo (struct ptpfs_sb_info *sb, __u32 handle,
                         struct ptp_object_info* objectinfo)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));

    ptp.code=PTP_OC_GetObjectInfo;
    ptp.param1=handle;
    ptp.nparam=1;
    ret=ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, &data);
    if (ret == PTP_RC_OK) ptp_unpack_OI(sb, &data, objectinfo);
    ptp_free_data_buffer(&data);
    return ret;
}


void ptp_free_data_buffer(struct ptp_data_buffer *buffer) 
{
    int x;
    for (x = 0; x < buffer->num_blocks; x++)
    {
        kfree(buffer->blocks[x].block);
    }
    kfree(buffer->blocks);
    buffer->blocks = 0;
    buffer->num_blocks = 0;
}


__u16
ptp_getobject (struct ptpfs_sb_info *sb, __u32 handle, struct ptp_data_buffer *data)
{
    struct ptp_container ptp;
    memset(&ptp,0,sizeof(ptp));

    ptp.code=PTP_OC_GetObject;
    ptp.param1=handle;
    ptp.nparam=1;
    return ptp_transaction(sb, &ptp, PTP_DP_GETDATA, 0, data);
}




/**
 * ptp_sendobjectinfo:
 * params:	PTPParams*
 *		__u32* store		- destination StorageID on Responder
 *		__u32* parenthandle 	- Parent ObjectHandle on responder
 * 		__u32* handle	- see Return values
 *		PTPObjectInfo* objectinfo- ObjectInfo that is to be sent
 * 
 * Sends ObjectInfo of file that is to be sent via SendFileObject.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : __u32* store	- Responder StorageID in which
 *					  object will be stored
 *		  __u32* parenthandle- Responder Parent ObjectHandle
 *					  in which the object will be stored
 *		  __u32* handle	- Responder's reserved ObjectHandle
 *					  for the incoming object
 **/
__u16
ptp_sendobjectinfo (struct ptpfs_sb_info *sb, __u32* store, 
                    __u32* parenthandle, __u32* handle,
                    struct ptp_object_info* objectinfo)
{
    __u16 ret;
    struct ptp_container ptp;
    struct ptp_data_buffer data;
    struct ptp_block block;

    memset(&ptp,0,sizeof(ptp));
    memset(&data,0,sizeof(data));

    if (ptp_operation_issupported(sb,PTP_OC_EK_SendFileObjectInfo))
    {
        ptp.code=PTP_OC_EK_SendFileObjectInfo;
    }
    else
    {
        ptp.code=PTP_OC_SendObjectInfo;
    }
    ptp.param1=*store;
    ptp.param2=*parenthandle;
    ptp.nparam=2;

    data.num_blocks = 1;
    data.blocks = &block;
    block.block_size = ptp_pack_OI(sb, objectinfo, &block.block);
    ret=ptp_transaction(sb, &ptp, PTP_DP_SENDDATA, block.block_size, &data); 
    kfree(block.block);

    *store=ptp.param1;
    *parenthandle=ptp.param2;
    *handle=ptp.param3; 

    return ret;
}

__u16 ptp_sendobject(struct ptpfs_sb_info *sb, struct ptp_data_buffer* object, __u32 size)
{
    struct ptp_container ptp;
    memset(&ptp,0,sizeof(ptp));

    if (ptp_operation_issupported(sb,PTP_OC_EK_SendFileObject))
    {
        ptp.code=PTP_OC_EK_SendFileObject;
    }
    else
    {
        ptp.code=PTP_OC_SendObject;
    }
    ptp.nparam=0;

    return ptp_transaction(sb, &ptp, PTP_DP_SENDDATA, size, object);
}

__u16 ptp_deleteobject(struct ptpfs_sb_info *sb, __u32 handle, __u32 ofc)
{
    struct ptp_container ptp;
    memset(&ptp,0,sizeof(ptp));
    ptp.code=PTP_OC_DeleteObject;
    ptp.param1=handle;
    ptp.param2=ofc;
    ptp.nparam=2;
    return ptp_transaction(sb, &ptp, PTP_DP_NODATA, 0, NULL);
}
