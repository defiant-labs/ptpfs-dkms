/**************************************************************************
	mmptp - command line utility for getting images from PTP cameras

	(c) 2002 - 2006 by Michael Minn (http://michaelminn.com)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

****************************************************************************/

#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <linux/usb.h>

#define VERSION_MESSAGE "MMPTP v2008.09.24 (c) 2002-2008 by Michael Minn\n"

/* Timeout in milliseconds for write operations */
#define WRITE_TIMEOUT		2000

/* Timeout in milliseconds for read operations - needs to be long enough for long transfers */
#define READ_TIMEOUT		20000


/* Container types */
#define MMPTP_CONTAINER_TYPE_UNDEFINED 0
#define MMPTP_CONTAINER_TYPE_COMMAND 1
#define MMPTP_CONTAINER_TYPE_DATA 2
#define MMPTP_CONTAINER_TYPE_RESPONSE 3
#define MMPTP_CONTAINER_TYPE_EVENT 4


/* Operation Codes - PIMA 15740:2000 spec, clause 10.4, pp 61 */

#define MMPTP_OPERATION_GETDEVICEINFO 0x1001
#define MMPTP_OPERATION_OPENSESSION 0x1002
#define MMPTP_OPERATION_CLOSESESSION 0x1003
#define MMPTP_OPERATION_GETSTORAGEIDS 0x1004
#define MMPTP_OPERATION_GETSTORAGEINFO 0x1005
#define MMPTP_OPERATION_GETNUMOBJECTS 0x1006
#define MMPTP_OPERATION_GETOBJECTHANDLES 0x1007
#define MMPTP_OPERATION_GETOBJECTINFO 0x1008
#define MMPTP_OPERATION_GETOBJECT 0x1009
#define MMPTP_OPERATION_GETTHUMB 0x100a
#define MMPTP_OPERATION_DELETEOBJECT 0x100b
#define MMPTP_OPERATION_SENDOBJECTINFO 0x100c
#define MMPTP_OPERATION_SENDOBJECT 0x100d
#define MMPTP_OPERATION_INITIATECAPTURE 0x100e
#define MMPTP_OPERATION_FORMATSTORE 0x100f
#define MMPTP_OPERATION_RESETDEVICE 0x1010
#define MMPTP_OPERATION_SELFTEST 0x1011
#define MMPTP_OPERATION_SETOJBJECTPROTECTION 0x1012
#define MMPTP_OPERATION_POWERDOWN 0x1013
#define MMPTP_OPERATION_GETDEVICEPROPDESC 0x1014
#define MMPTP_OPERATION_GETDEVICEPROPVALUE 0x1015
#define MMPTP_OPERATION_SETDEVICEPROPVALUE 0x1016
#define MMPTP_OPERATION_RESETDEVICEPROPVALUE 0x1017
#define MMPTP_OPERATION_TERMINATEOPENCAPTURE 0x1018
#define MMPTP_OPERATION_MOVEOBJECT 0x1019
#define MMPTP_OPERATION_COPYOBJECT 0x101a
#define MMPTP_OPERATION_GETPARTIALOBJECT 0x101b
#define MMPTP_OPERATION_INITIATEOPENCAPTURE 0x101c


/* Response Codes - PIMA 15740:2000 spec, clause 11, pp 85 */

#define MMPTP_RESPONSE_OK 0x2001
#define MMPTP_RESPONSE_GENERAL_ERROR 0x2002
#define MMPTP_RESPONSE_SESSION_NOT_OPEN 0x2003
#define MMPTP_RESPONSE_INVALID_TRANSACTION_ID 0x2004
#define MMPTP_RESPONSE_OPERATION_NOT_SUPPORTED 0x2005
#define MMPTP_RESPONSE_PARAMETER_NOT_SUPPORTED 0x2006
#define MMPTP_RESPONSE_INCOMPLETE_TRANSFER 0x2007
#define MMPTP_RESPONSE_INVALID_STORAGE_ID 0x2008
#define MMPTP_RESPONSE_INVALID_OBJECTHANDLE 0x2009
#define MMPTP_RESPONSE_DEVICEPROP_NOT_SUPPORTED 0x200a
#define MMPTP_RESPONSE_INVALID_OBJECTFORMAT_CODE 0x200b
#define MMPTP_RESPONSE_STORE_FULL 0x200c
#define MMPTP_RESPONSE_OBJECT_WRITE_PROTECTED 0x200d
#define MMPTP_RESPONSE_STORE_READ_ONLY 0x200e
#define MMPTP_RESPONSE_ACCESS_DENIED 0x200f
#define MMPTP_RESPONSE_NO_THUMBNAIL_PRESENT 0x2010
#define MMPTP_RESPONSE_SELFTEST_FAILED 0x2011
#define MMPTP_RESPONSE_PARTIAL_DELETION 0x2012
#define MMPTP_RESPONSE_STORE_NOT_AVAILABLE 0x2013
#define MMPTP_RESPONSE_SPECIFICATION_UNSUPPORTED 0x2014
#define MMPTP_RESPONSE_NO_VALID_OBJECTINFO 0x2015
#define MMPTP_RESPONSE_INVALID_CODE_FORMAT 0x2016
#define MMPTP_RESPONSE_UNKNOWN_VENDOR_CODE 0x2017
#define MMPTP_RESPONSE_CAPTURE_ALREADY_TERMINATED 0x2018
#define MMPTP_RESPONSE_DEVICE_BUSY 0x2019
#define MMPTP_RESPONSE_INVALID_PARENTOBJECT 0x201a
#define MMPTP_RESPONSE_INVALID_DEVICEPROP_FORMAT 0x201b
#define MMPTP_RESPONSE_INVALID_DEVICEPROP_VALUE 0x201c
#define MMPTP_RESPONSE_INVALID_PARAMETER 0x201d
#define MMPTP_RESPONSE_SESSION_ALREADY_OPEN 0x201e
#define MMPTP_RESPONSE_TRANSACTION_CANCELLED 0x201f
#define MMPTP_RESPONSE_DESTINATION_UNSUPPORTED 0x2020




/* Event codes - PIMA 15740:2000 spec, clause 12.4, pp93 */

#define MMPTP_EVENT_CANCELTRANSACTION 0x4001
#define MMPTP_EVENT_OBJECTADDED 0x4002
#define MMPTP_EVENT_OBJECTREMOVED 0x4003
#define MMPTP_EVENT_STOREADDED 0x4004
#define MMPTP_EVENT_STOREREMOVED 0x4005
#define MMPTP_EVENT_DEVICEPROPCHANGED 0x4006
#define MMPTP_EVENT_OBJECTINFOCHANGED 0x4007
#define MMPTP_EVENT_DEVICEINFOCHANGED 0x4008
#define MMPTP_EVENT_REQUESTOBJECTTRANSFER 0x4009
#define MMPTP_EVENT_STOREFULL 0x400a
#define MMPTP_EVENT_DEVICERESET 0x400b
#define MMPTP_EVENT_STORAGEINFOCHANGED 0x400c
#define MMPTP_EVENT_CAPTURECOMPLETE 0x400d
#define MMPTP_EVENT_UNREPORTEDSTATUS 0x400e


/* Image Property Codes - PIMA 15740:2000 spec, clause 13.3.5, pp29 */

#define MMPTP_PROPERTY_BATTERYLEVEL 0x5001
#define MMPTP_PROPERTY_FUNCTIONALMODE 0x5002
#define MMPTP_PROPERTY_IMAGESIZE 0x5003
#define MMPTP_PROPERTY_COMPRESSIONSETTING 0x5004
#define MMPTP_PROPERTY_WHITEBALANCE 0x5005
#define MMPTP_PROPERTY_RGBGAIN 0x5006
#define MMPTP_PROPERTY_FNUMBER 0x5007
#define MMPTP_PROPERTY_FOCALLENGTH 0x5008
#define MMPTP_PROPERTY_FOCUSDISTANCE 0x5009
#define MMPTP_PROPERTY_FOCUSMODE 0x500a
#define MMPTP_PROPERTY_EXPOSUREMETERINGMODE 0x500b
#define MMPTP_PROPERTY_FLASHMODE 0x500c
#define MMPTP_PROPERTY_EXPOSURETIME 0x500d
#define MMPTP_PROPERTY_EXPOSUREPROGRAMMODE 0x500e
#define MMPTP_PROPERTY_EXPOSUREINDEX 0x500f
#define MMPTP_PROPERTY_EXPOSUREBIASCOMPENSATION 0x5010
#define MMPTP_PROPERTY_DATETIME 0x5011
#define MMPTP_PROPERTY_CAPTUREDELAY 0x5012
#define MMPTP_PROPERTY_STILLCAPTUREMODE 0x5013
#define MMPTP_PROPERTY_CONTRAST 0x5014
#define MMPTP_PROPERTY_SHARPNESS 0x5015
#define MMPTP_PROPERTY_DIGITALZOOM 0x5016
#define MMPTP_PROPERTY_EFFECTMODE 0x5017
#define MMPTP_PROPERTY_BURSTNUMBER 0x5018
#define MMPTP_PROPERTY_BURSTINTERVAL 0x5019
#define MMPTP_PROPERTY_TIMELAPSENUMBER 0x501a
#define MMPTP_PROPERTY_TIMELAPSEINTERVAL 0x501b
#define MMPTP_PROPERTY_FOCUSMETERINGMODE 0x501c
#define MMPTP_PROPERTY_UPLOADURL 0x501d
#define MMPTP_PROPERTY_ARTIST 0x501e
#define MMPTP_PROPERTY_COPYRIGHTINFO 0x501f


/****************************** Unclassed Utilities ******************************/

void hex_dump(unsigned char *data, int length)
{
	for (int x = 0; data && (x < length) && (x < 512); x += 16)
	{
		fprintf(stderr, "%04x ", x);
		for (int y = 0; (y < 16) && ((x + y) < length); ++y)
			fprintf(stderr, "%02x ", data[x + y]);
		
		for (int y = 0; (y < 16) && ((x + y) < length); ++y)
			if ((data[x + y] >= ' ') && (data[x + y] <= '~'))
				fprintf(stderr, "%c", data[x + y]);
			else
				fprintf(stderr, ".");
		fprintf(stderr, "\n");
	}
}

char *unicode_to_ascii(unsigned char *unicode)
{
	static char ascii[257];
	memset(ascii, 0, sizeof(ascii));

	if (!unicode)
		return ascii;

	for (int scan = 0; scan < *unicode; ++scan)
	{
		ascii[scan] = (char) unicode[1 + (scan * 2)];
		if (ascii[scan] <= ' ')
			ascii[scan] = ' ';
	}
	return ascii;
}

int char_to_int16(unsigned char *scan)
{
	return (int) scan[0] + ((int) scan[1] << 8);
}

int int16_to_char(int integer, unsigned char *data)
{
	data[0] = (integer & 0xff);
	data[1] = ((integer >> 8) & 0xff);
	return integer;
}

int char_to_int32(unsigned char *scan)
{
	return (int) scan[0] + ((int) scan[1] << 8) + ((int) scan[2] << 16) + ((int) scan[3] << 24);
}

int int32_to_char(int integer, unsigned char *data)
{
	data[0] = (integer & 0xff);
	data[1] = ((integer >> 8) & 0xff);
	data[2] = ((integer >> 16) & 0xff);
	data[3] = ((integer >> 24) & 0xff);
	return integer;
}

double char_to_int64(unsigned char *scan)
{
	int low = (int) scan[0] + ((int) scan[1] << 8) + ((int) scan[2] << 16) + ((int) scan[3] << 24);
	int high = (int) scan[4] + ((int) scan[5] << 8) + ((int) scan[6] << 16) + ((int) scan[7] << 24);
	return low + (high * 4294967296.0);
}

char *response_code_string(int code)
{
	switch (code)
	{
		case MMPTP_RESPONSE_OK: return "OK";
		case MMPTP_RESPONSE_GENERAL_ERROR: return "General Error";
		case MMPTP_RESPONSE_SESSION_NOT_OPEN: return "Session Not Open";
		case MMPTP_RESPONSE_INVALID_TRANSACTION_ID: return "Invalid Transaction ID";
		case MMPTP_RESPONSE_OPERATION_NOT_SUPPORTED: return "Operation Not Supported";
		case MMPTP_RESPONSE_PARAMETER_NOT_SUPPORTED: return "Parameter Not Supported";
		case MMPTP_RESPONSE_INCOMPLETE_TRANSFER: return "Incomplete Transfer";
		case MMPTP_RESPONSE_INVALID_STORAGE_ID: return "Invalid Storage ID";
		case MMPTP_RESPONSE_INVALID_OBJECTHANDLE: return "Invalid Object Handle";
		case MMPTP_RESPONSE_DEVICEPROP_NOT_SUPPORTED: return "Device Property Not Supported";
		case MMPTP_RESPONSE_INVALID_OBJECTFORMAT_CODE: return "Invalid Object Format Code";
		case MMPTP_RESPONSE_STORE_FULL: return "Store Full";
		case MMPTP_RESPONSE_OBJECT_WRITE_PROTECTED: return "Object Write Protected";
		case MMPTP_RESPONSE_STORE_READ_ONLY: return "Store Is Read Only";
		case MMPTP_RESPONSE_ACCESS_DENIED: return "Access Denied";
		case MMPTP_RESPONSE_NO_THUMBNAIL_PRESENT: return "No Thumbnail Present";
		case MMPTP_RESPONSE_SELFTEST_FAILED: return "Self Test Failed";
		case MMPTP_RESPONSE_PARTIAL_DELETION: return "Partial Deletion";
		case MMPTP_RESPONSE_STORE_NOT_AVAILABLE: return "Store Not Available";
		case MMPTP_RESPONSE_SPECIFICATION_UNSUPPORTED: return "Specification By Format Unsupported";
		case MMPTP_RESPONSE_NO_VALID_OBJECTINFO: return "No Valid Object Info";
		case MMPTP_RESPONSE_INVALID_CODE_FORMAT: return "Invalid Code Format";
		case MMPTP_RESPONSE_UNKNOWN_VENDOR_CODE: return "Unknown Vendor Code";
		case MMPTP_RESPONSE_CAPTURE_ALREADY_TERMINATED: return "Capture Already Terminated";
		case MMPTP_RESPONSE_DEVICE_BUSY: return "Device Busy";
		case MMPTP_RESPONSE_INVALID_PARENTOBJECT: return "Invalid Parent Object";
		case MMPTP_RESPONSE_INVALID_DEVICEPROP_FORMAT: return "Invalid Device Property Format";
		case MMPTP_RESPONSE_INVALID_DEVICEPROP_VALUE: return "Invalid Device Property Value";
		case MMPTP_RESPONSE_INVALID_PARAMETER: return "Invalid Parameter";
		case MMPTP_RESPONSE_SESSION_ALREADY_OPEN: return "Session Already Open";
		case MMPTP_RESPONSE_TRANSACTION_CANCELLED: return "Transaction Cancelled";
		case MMPTP_RESPONSE_DESTINATION_UNSUPPORTED: return "Destination Unsupported";
	}

	static char default_message[32];
	sprintf(default_message, "Invalid Response %x", code);
	return default_message;
}

char *storage_type_string(int code)
{
	switch (code)
	{
		case 0: return "Undefined";
		case 1: return "Fixed ROM";
		case 2: return "Removable ROM";
		case 3: return "Fixed RAM";
		case 4: return "Removable RAM";
	}
	return "(invalid code)";
}

char *device_property_string(int code)
{
	switch (code)
	{
		case MMPTP_PROPERTY_BATTERYLEVEL: return "Battery Level"; break;
		case MMPTP_PROPERTY_FUNCTIONALMODE: return "Functional Mode"; break;
		case MMPTP_PROPERTY_IMAGESIZE: return "Image Size"; break;
		case MMPTP_PROPERTY_COMPRESSIONSETTING: return "Compression Setting"; break;
		case MMPTP_PROPERTY_WHITEBALANCE: return "White Balance"; break;
		case MMPTP_PROPERTY_RGBGAIN: return "RGB Gain"; break;
		case MMPTP_PROPERTY_FNUMBER: return "F-Stop"; break;
		case MMPTP_PROPERTY_FOCALLENGTH: return "Focal Length"; break;
		case MMPTP_PROPERTY_FOCUSDISTANCE: return "Focus Distance"; break;
		case MMPTP_PROPERTY_FOCUSMODE: return "Focus Mode"; break;
		case MMPTP_PROPERTY_EXPOSUREMETERINGMODE: return "Exposure Metering Mode"; break;
		case MMPTP_PROPERTY_FLASHMODE: return "Flash Mode"; break;
		case MMPTP_PROPERTY_EXPOSURETIME: return "Exposure Time"; break;
		case MMPTP_PROPERTY_EXPOSUREPROGRAMMODE: return "Exposure Program Mode"; break;
		case MMPTP_PROPERTY_EXPOSUREINDEX: return "Exposure Index"; break;
		case MMPTP_PROPERTY_EXPOSUREBIASCOMPENSATION: return "Exposure Bias Compensation"; break;
		case MMPTP_PROPERTY_DATETIME: return "Date/Time"; break;
		case MMPTP_PROPERTY_CAPTUREDELAY: return "Capture Delay"; break;
		case MMPTP_PROPERTY_STILLCAPTUREMODE: return "Still Capture Mode"; break;
		case MMPTP_PROPERTY_CONTRAST: return "Contrast"; break;
		case MMPTP_PROPERTY_SHARPNESS: return "Sharpness"; break;
		case MMPTP_PROPERTY_DIGITALZOOM: return "Digital Zoom"; break;
		case MMPTP_PROPERTY_EFFECTMODE: return "Effect Mode"; break;
		case MMPTP_PROPERTY_BURSTNUMBER: return "Burst Number"; break;
		case MMPTP_PROPERTY_BURSTINTERVAL: return "Burst Interval"; break;
		case MMPTP_PROPERTY_TIMELAPSENUMBER: return "Time Lapse Number"; break;
		case MMPTP_PROPERTY_TIMELAPSEINTERVAL: return "Time Lapse Interval"; break;
		case MMPTP_PROPERTY_FOCUSMETERINGMODE: return "Focus Metering Mode"; break;
		case MMPTP_PROPERTY_UPLOADURL: return "Upload URL"; break;
		case MMPTP_PROPERTY_ARTIST: return "Artist"; break;
		case MMPTP_PROPERTY_COPYRIGHTINFO: return "Copyright Info"; break;
	}

	static char default_message[32];
	sprintf(default_message, "Invalid Device Property %x", code);
	return default_message;
}

char *filesystem_type_string(int code)
{
	switch (code)
	{
		case 0: return "Undefined";
		case 1: return "Generic Flat";
		case 2: return "Generic Hierarchical";
		case 3: return "DCF";
	}

	return "Vendor-specific";
}

/* Image Formats - PIMA 15740:2000 spec, clause 6.2, pp 37 */
char *object_format_string(int code)
{
	switch (code)
	{
		case 0x3000: return "Undefined";
		case 0x3001: return "Folder";
		case 0x3002: return "Script";
		case 0x3003: return "Executable";
		case 0x3004: return "Text";
		case 0x3005: return "HTML";
		case 0x3006: return "Digital Print Order Format (DPOF)";
		case 0x3007: return "AIFF";
		case 0x3008: return "WAV";
		case 0x3009: return "MP3";
		case 0x300a: return "AVI";
		case 0x300b: return "MPEG";
		case 0x300c: return "ASF";
		case 0x300d: return "QuickTime MOV";
		case 0x3801: return "EXIF/JPEG";
		case 0x3802: return "TIFF/EP";
		case 0x3803: return "FlashPix";
		case 0x3804: return "BMP";
		case 0x3805: return "CIFF";
		case 0x3807: return "GIF";
		case 0x3808: return "JFIF";
		case 0x3809: return "PhotoCD Image Pac";
		case 0x380a: return "Quickdraw PICT";
		case 0x380b: return "PNG";
		case 0x380d: return "TIFF";
		case 0x380e: return "TIFF/IT";
		case 0x380f: return "JPEG2000";
		case 0x3810: return "JPEG2000 Extended";
	}

	static char string[16] = "";
	sprintf(string, "%x", code);
	return string;
}

char *object_format_extension(int code)
{
	switch (code)
	{
		case 0x3000: return ".dat";
		case 0x3001: return "";
		case 0x3002: return ".bat";
		case 0x3003: return ".exe";
		case 0x3004: return ".txt";
		case 0x3005: return ".htm";
		case 0x3006: return ".dpof";
		case 0x3007: return ".aiff";
		case 0x3008: return ".wav";
		case 0x3009: return ".mp3";
		case 0x300a: return ".avi";
		case 0x300b: return ".mpg";
		case 0x300c: return ".asf";
		case 0x300d: return ".mov";
		case 0x3801: return ".jpg";
		case 0x3802: return ".tiff";
		case 0x3803: return ".fp";
		case 0x3804: return ".bmp";
		case 0x3805: return ".ciff";
		case 0x3807: return ".gif";
		case 0x3808: return ".jpg";
		case 0x3809: return ".pcip";
		case 0x380a: return ".pict";
		case 0x380b: return ".png";
		case 0x380d: return ".tiff";
		case 0x380e: return ".tiff";
		case 0x380f: return ".jpg";
		case 0x3810: return ".jpg";
	}

	return ".dat";
}

/****************************** MMPTP Class ******************************/

class mmptp
{
	public:
	mmptp();
	~mmptp() { release_camera(); }
	int print_device_info();
	int list_usb_devices();
	int list_images(int extract);
	int reset_camera();
	int acquire_camera();
	int release_camera();

	private:
	struct usb_device *device;
	usb_dev_handle *handle;
	int write_endpoint;
	int read_endpoint;
	int transaction_id;

	int transaction(int operation, int parameter = -1, unsigned char **return_data = NULL, int return_length = 0);
	int transaction_send_data(int operation, int parameter, unsigned char *data, int data_length);
};

mmptp::mmptp()
{
	memset(this, 0, sizeof(*this));

    	usb_init();
    	int bus_count = usb_find_busses();
	if (bus_count < 0)
	{
		perror("usb_find_busses()");
		return;
	}

	int device_count = usb_find_devices();
	if (device_count < 0)
	{
		perror("usb_find_devices()");
		return;
	}

	printf("%d busses, %d devices\n", bus_count, device_count);
}

int mmptp::acquire_camera()
{
	struct usb_bus *busses = usb_get_busses();
	for (struct usb_bus *bus = busses; bus; bus = bus->next)
    		for (device = bus->devices; device; device = device->next)
			for (int configuration = 0; configuration < 
					device->descriptor.bNumConfigurations; ++configuration)
				for (int interface = 0; interface < device->config[configuration].
						bNumInterfaces; ++interface)
					for (int alternate = 0; alternate < device->config[configuration].
							interface[alternate].num_altsetting; ++alternate)
						if (device->config[configuration].interface[interface].
							altsetting[alternate].bInterfaceClass == 6)
						{
							handle = usb_open(device);
							if (!handle)
							{
								int status = -errno;
								perror("usb_open() failure");
								device = NULL;
								return status;
							}
							if (usb_set_configuration(handle, device->config[configuration].
								bConfigurationValue) < 0)
							{
								int status = -errno;
								perror("usb_set_configuration() failure");
								usb_close(handle);
								handle = NULL;
								device = NULL;
								return status;
							}
							if (usb_claim_interface(handle, device->config[configuration].
								interface[interface].altsetting[alternate].
								bInterfaceNumber) < 0)
							{
								int status = -errno;
								perror("usb_claim_interface() failure");
								usb_close(handle);
								handle = NULL;
								device = NULL;
								return status;
							}
							if (usb_set_altinterface(handle, device->config[configuration].
								interface[interface].altsetting[alternate].
								bAlternateSetting) < 0)
							{
								int status = -errno;
								perror("usb_set_altinterface() failure");
								usb_close(handle);
								handle = NULL;
								device = NULL;
								return status;
							}

							for (int endpoint = 0; endpoint < device->config[configuration].
									interface[interface].altsetting[alternate].
									bNumEndpoints; ++endpoint)
							{
								int address = device->config[configuration].
									interface[interface].altsetting[alternate].
									endpoint[endpoint].bEndpointAddress;
								int packet_size = device->config[configuration].
									interface[interface].altsetting[alternate].
									endpoint[endpoint].wMaxPacketSize;
								int attributes = device->config[configuration].
									interface[interface].altsetting[alternate].
									endpoint[endpoint].bmAttributes;

								if (attributes == USB_ENDPOINT_TYPE_BULK)
									if (address & USB_ENDPOINT_DIR_MASK)
										read_endpoint = address;
									else
										write_endpoint = address;
							}

							return 1;
						}

	device = NULL;
	handle = NULL;
	return -ENODEV;
}

int mmptp::release_camera()
{
	if (handle)
		usb_close(handle);
	handle = NULL;
	device = NULL;
}

int mmptp::list_usb_devices()
{
	struct usb_bus *busses = usb_get_busses();
	for (struct usb_bus *bus = busses; bus; bus = bus->next)
	{
		printf("Bus %s\n", bus->dirname);
    		for (struct usb_device *device = bus->devices; device; device = device->next)
		{
			printf("  Device %s (class 0x%x)\n", device->filename, device->descriptor.bDeviceClass);
			usb_dev_handle *device_handle = usb_open(device);
			if (device_handle)
			{
				char product[128] = "";
				char manufacturer[128] = "";
				char serial_number[128] = "";
				int status = usb_get_string_simple(device_handle, device->descriptor.iProduct, 
					product, sizeof(product));
				usb_get_string_simple(device_handle, device->descriptor.iManufacturer, 
					manufacturer, sizeof(manufacturer));
				usb_get_string_simple(device_handle, device->descriptor.iSerialNumber, 
					serial_number, sizeof(serial_number));
				printf("    Product:       %s\n", product);
				printf("    Manufacturer:  %s\n", manufacturer);
				printf("    Serial Number: %s\n", serial_number);

				for (int configuration = 0; configuration < 
						device->descriptor.bNumConfigurations; ++configuration)
					for (int interface = 0; interface < device->config[configuration].
							bNumInterfaces; ++interface)
						for (int altsetting = 0; altsetting < device->config[configuration].
								interface[interface].num_altsetting; ++altsetting)
						{
							printf("      Interface:   %x:%x:%x (class 0x%x)\n",
								configuration, interface, altsetting,
    								device->config[configuration].interface[interface].
								altsetting[altsetting].bInterfaceClass);
						}

				usb_close(device_handle);
			}
		}
	}		
}

int mmptp::transaction(int operation, int parameter, unsigned char **return_data, int return_length)
{
	if (!this || !handle)
		return -EINVAL;

	if (return_data)
		*return_data = NULL;

	unsigned char request[24]; // 12-byte header + 3 x 4-byte parameters
	int length = 12;
	if (parameter >= 0)
		length = 16;

	// 12-byte header - integers are LSB first (little endian)
	++transaction_id;
	int32_to_char(length, request);
	int16_to_char(MMPTP_CONTAINER_TYPE_COMMAND, request + 4);
	int16_to_char(operation, request + 6);
	int32_to_char(transaction_id, request + 8);

	if (parameter >= 0)
		int32_to_char(parameter, request + 12);

	//fprintf(stderr, "\ntransaction(endpoint %02x, command 0x%x, parameter 0x%x, length %d, trans id %d)\n", 
	//	write_endpoint, operation, parameter, length, transaction_id);

	if (usb_bulk_write(handle, write_endpoint, (char*) request, length, WRITE_TIMEOUT) < 0)
	{
		int status = -errno;
		perror("usb_bulk_write() failure sending transaction request");
		return status;
	}

	// 11/7/2007 - usb_bulk_read() must read complete object in one read. Therefore,
	// this parameter is used to pass image size when downloading images that can be very large.

	if (return_length <= 0)
		return_length = 16777216; // arbitrary large number
	else
		return_length += 1024;
		
	unsigned char *response = new unsigned char[return_length];
	memset(response, 0, 12);
	if (!response)
	{
		int status = -errno;
		fprintf(stderr, "Failure allocating %d byte return data buffer: %s", return_length, strerror(errno));
		return status;
	}	

	int read_status = 0;
	if ((read_status = usb_bulk_read(handle, read_endpoint, (char*) response, return_length, READ_TIMEOUT)) < 0)
	{
		int status = -errno;
		perror("Response usb_bulk_read() failure getting transaction response");
		return status;
	}

	length = char_to_int32(response);
	int type = char_to_int16(response + 4);
	int code = char_to_int16(response + 6);
	int id = char_to_int32(response + 8);

	//fprintf(stderr, "Transaction response: status %d, %d bytes, type %d, code 0x%x, trans id %d\n", 
	//	read_status, length, type, code, id);
	//hex_dump(response, 128);

	if (read_status <= 0)
	{
		fprintf(stderr, "Failure reading transaction response from device: status %d, operation 0x%x\n", 
			read_status, code);
		return -EBADMSG;
	}
	
	else if (type == MMPTP_CONTAINER_TYPE_RESPONSE)
	{
		//fprintf(stderr, "Transaction complete\n\n");

		if (code != MMPTP_RESPONSE_OK)
		{
			fprintf(stderr, "Transaction failed: %s (operation 0x%x, length %d, type %d, code %d, id %d)\n", 
				response_code_string(code), operation, length, type, code, id);
			return -code;
		}

		return code;
	}

	if (type != MMPTP_CONTAINER_TYPE_DATA)
	{
		fprintf(stderr, "Invalid container type 0x%x for data returned from device (length %d)\n", type, length);
		return -EBADMSG;
	}

	if (!return_data)
	{
		fprintf(stderr, "No parameter passed for response data\n");
		return -EINVAL;
	}

	*return_data = new unsigned char[length];
	if (!*return_data)
	{
		int status = -errno;
		fprintf(stderr, "Failure allocating %d byte return data buffer: %s", length, strerror(errno));
		return status;
	}

	memcpy(*return_data, response + 12, length - 12);

	// A DATA transfer is followed by a RESPONSE transfer
	if (usb_bulk_read(handle, read_endpoint, (char*) response, 24, 10000) < 0)
	{
		int status = -errno;
		perror("Data usb_bulk_read() failure waiting for response");
		delete[] response;
		delete[] *return_data;
		*return_data = NULL;
		return status;
	}

	int response_length = char_to_int32(response);
	type = char_to_int16(response + 4);
	code = char_to_int16(response + 6);
	id = char_to_int32(response + 8);
	delete[] response;

	//fprintf(stderr, "  Response URB %d bytes, type %d, code 0x%x, id 0x%x\n", response_length, type, code, id);

	if ((type != MMPTP_CONTAINER_TYPE_RESPONSE) || (code != MMPTP_RESPONSE_OK))
	{
		fprintf(stderr, "Transaction failure: %s (operation 0x%x, length %d, type 0x%x, code 0x%x)\n\n", 
			response_code_string(code), operation, response_length, type, code);
		delete[] *return_data;
		*return_data = NULL;
		return -code;
	}

	return length - 12;
}

// For SET transactions that require sending data to the camera
int mmptp::transaction_send_data(int operation, int parameter, unsigned char *data, int data_length)
{
	if (!this || !handle || !data || (data_length <= 0))
		return -EINVAL;

	unsigned char request[24]; // 12-byte header + 3 x 4-byte parameters
	int length = 12;
	if (parameter >= 0)
		length = 16;

	// 12-byte header - integers are LSB first (little endian)
	++transaction_id;
	int32_to_char(length, request);
	int16_to_char(MMPTP_CONTAINER_TYPE_COMMAND, request + 4);
	int16_to_char(operation, request + 6);
	int32_to_char(transaction_id, request + 8);

	if (parameter >= 0)
		int32_to_char(parameter, request + 12);

	fprintf(stderr, "\ntransaction(endpoint %02x, command 0x%x, parameter 0x%x, length %d, trans id %d)\n", 
		write_endpoint, operation, parameter, length, transaction_id);

	if (usb_bulk_write(handle, write_endpoint, (char*) request, length, WRITE_TIMEOUT) < 0)
	{
		int status = -errno;
		perror("usb_bulk_write() failure sending transaction request");
		return status;
	}

	length = 12 + data_length;
	unsigned char *data_container = new unsigned char[length];
	int32_to_char(length, data_container);
	int16_to_char(MMPTP_CONTAINER_TYPE_DATA, data_container + 4);
	int16_to_char(operation, data_container + 6);
	int32_to_char(transaction_id, data_container + 8);
	memcpy(data_container + 12, data, data_length);

	if (usb_bulk_write(handle, write_endpoint, (char*) data_container, length, WRITE_TIMEOUT) < 0)
	{
		int status = -errno;
		perror("usb_bulk_write() failure sending transaction data");
		return status;
	}

	fprintf(stderr, "  Sent %d byte data container\n", 12 + data_length);

	int read_status = 0;
	unsigned char response[12];
	if ((read_status = usb_bulk_read(handle, read_endpoint, (char*) response, 12, READ_TIMEOUT)) < 0)
	{
		int status = -errno;
		perror("Response usb_bulk_read() failure getting transaction response");
		return status;
	}

	length = char_to_int32(response);
	int type = char_to_int16(response + 4);
	int code = char_to_int16(response + 6);
	int id = char_to_int32(response + 8);

	if (read_status <= 0)
	{
		fprintf(stderr, "Failure reading transaction response from device: status %d, operation 0x%x\n", 
			read_status, code);
		return -EBADMSG;
	}
	
	else if (type != MMPTP_CONTAINER_TYPE_RESPONSE)
	{
		fprintf(stderr, "Invalid container type 0x%x for response from device (length %d)\n", type, length);
		return -EBADMSG;
	}

	else if (code != MMPTP_RESPONSE_OK)
	{
		fprintf(stderr, "Transaction failed: %s (operation 0x%x, length %d, type %d, code %d, id %d)\n", 
			response_code_string(code), operation, length, type, code, id);
		return -code;
	}

	return code;
}

int mmptp::print_device_info()
{
	if (!this || !handle)
		return -EINVAL;

	unsigned char *device_info = NULL;
	int status = transaction(MMPTP_OPERATION_GETDEVICEINFO, -1, &device_info);
	if (status < 0)
		return status;

	if (device_info)
	{
		printf("Device information ----------\n");

		unsigned char *standard_version = device_info;
		unsigned char *vendor_extension_id = device_info + 2;
		unsigned char *vendor_extension_version = device_info + 6;
		unsigned char *vendor_extension_description = device_info + 8;
		unsigned char *functional_mode = vendor_extension_description + 1 + (vendor_extension_description[0] * 2);
		unsigned char *operations_supported = functional_mode + 2;
		unsigned char *events_supported = operations_supported + 4 + (2 * char_to_int32(operations_supported));
		unsigned char *properties_supported = events_supported + 4 + (2 * char_to_int32(events_supported));
		unsigned char *capture_formats = properties_supported + 4 + (2 * char_to_int32(properties_supported));
		unsigned char *image_formats = capture_formats + 4 + (2 * char_to_int32(capture_formats));
		unsigned char *manufacturer = image_formats + 4 + (2 * char_to_int32(image_formats));
		unsigned char *model = manufacturer + 1 + (manufacturer[0] * 2);
		unsigned char *device_version = model + 1 + (model[0] * 2);
		unsigned char *serial_number = device_version + 1 + (device_version[0] * 2);

		printf("  Standard Version:     %f\n", char_to_int16(standard_version) / 100.0);
		printf("  Vendor Extension ID:  0x%x\n", char_to_int32(vendor_extension_id));
		printf("  Vendor Ext Version:   %f\n", char_to_int16(vendor_extension_version) / 100.0);
		printf("  Vendor Ext Desc:      %s", unicode_to_ascii(vendor_extension_description));
		printf("  Functional Mode:      %d\n", char_to_int16(functional_mode));
		printf("  Operations:           ");
		for (int op = 0; op < char_to_int32(operations_supported); ++op)
			printf("%x ", char_to_int16(operations_supported + 4 + (2 * op)));
		printf("\n");

		printf("Events:                 ");
		for (int op = 0; op < char_to_int32(events_supported); ++op)
			printf("%x ", char_to_int16(events_supported + 4 + (2 * op)));
		printf("\n");

		printf("Device Properties:      ");
		for (int op = 0; op < char_to_int32(properties_supported); ++op)
			printf("%x ", char_to_int16(properties_supported + 4 + (2 * op)));
		printf("\n");

		printf("Capture Formats:        ");
		for (int op = 0; op < char_to_int32(capture_formats); ++op)
			printf("%x ", char_to_int16(capture_formats + 4 + (2 * op)));
		printf("\n");

		printf("Image Formats:          ");
		for (int op = 0; op < char_to_int32(image_formats); ++op)
			printf("%s ", object_format_string(char_to_int16(image_formats + 4 + (2 * op))));
		printf("\n");

		printf("Manufacturer:           %s\n", unicode_to_ascii(manufacturer));
		printf("Model:                  %s\n", unicode_to_ascii(model));
		printf("Device Version:         %s\n", unicode_to_ascii(device_version));
		printf("Serial Number:          %s\n", unicode_to_ascii(serial_number));

		delete[] device_info;
	}

	int session_id = time(NULL);
	status = transaction(MMPTP_OPERATION_OPENSESSION, session_id);
	if (status < 0)
		return status;

	unsigned char *storage_ids = NULL;
	status = transaction(MMPTP_OPERATION_GETSTORAGEIDS, -1, &storage_ids);
	if (status < 0)
		return status;

	if (storage_ids)
	{
		printf("\nStorage Information ------------------\n", status);
		for (unsigned char *id = storage_ids + 4, *end = storage_ids + status; id < end; id += 4)
		{
			printf("  Storage ID %x\n", char_to_int32(id));
	
			unsigned char *storage_info = NULL;
			status = transaction(MMPTP_OPERATION_GETSTORAGEINFO, char_to_int32(id), &storage_info);
			if (storage_info)
			{
				unsigned char *storage = storage_info;
				unsigned char *filesystem = storage_info + 2;
				unsigned char *access = filesystem + 2;
				unsigned char *capacity = access + 2;
				unsigned char *free_bytes = capacity + 8;
				unsigned char *free_images = free_bytes + 8;
				unsigned char *description = free_images + 4;
				unsigned char *volume = description + 1 + ((int)description[0] * 2);
			
				printf("    Storage type:         %s\n", storage_type_string(char_to_int16(storage)));
				printf("    File system type:     %s\n", filesystem_type_string(char_to_int16(filesystem)));
				printf("    Access capability:    %x\n", char_to_int16(access));
				printf("    Size:                 %.0f bytes\n", char_to_int64(capacity));
				printf("    Free space:           %.0f bytes\n", char_to_int64(free_bytes));
				printf("    Free images:          %d\n", char_to_int32(free_images));
				printf("    Storage description:  %s\n", unicode_to_ascii(description));
				printf("    Volume label:         %s\n", unicode_to_ascii(volume));

				delete[] storage_info;
			}
		}

		delete[] storage_ids;
	}

	printf("\nDevice Properties ------------------\n", status);

	// These are a pair of commonly supported device properties
	unsigned char *response = NULL;
	status = transaction(MMPTP_OPERATION_GETDEVICEPROPVALUE, MMPTP_PROPERTY_BATTERYLEVEL, &response);
	if ((status > 0) && response)
		printf("    Battery level:        %d%%\n", response[0]);
	
	status = transaction(MMPTP_OPERATION_GETDEVICEPROPVALUE, MMPTP_PROPERTY_DATETIME, &response);
	if ((status > 0) && response)
		printf("    Date/Time:            %s\n", unicode_to_ascii(response));

	/**
	status = transaction(MMPTP_OPERATION_GETDEVICEPROPVALUE, MMPTP_PROPERTY_FNUMBER, &response);

	unsigned char fstop[2] = { 100, 0 };
	status = transaction_send_data(MMPTP_OPERATION_SETDEVICEPROPVALUE, MMPTP_PROPERTY_FNUMBER, fstop, 2);
	fprintf(stderr, "F-Stop: 0x%x\n", status);
	**/

	//hex_dump(response, status);

	status = transaction(MMPTP_OPERATION_CLOSESESSION);

	release_camera();
		
	return status;
}


#if 0

T:  Bus=03 Lev=01 Prnt=01 Port=00 Cnt=01 Dev#=  2 Spd=12  MxCh= 0
D:  Ver= 2.00 Cls=00(>ifc ) Sub=00 Prot=00 MxPS= 8 #Cfgs=  1
P:  Vendor=040a ProdID=0570 Rev= 1.00
S:  Manufacturer=Eastman Kodak Company
S:  Product=KODAK EasyShare DX6340 Zoom Digital Camera
S:  SerialNumber=KCKCJ33010636
C:* #Ifs= 1 Cfg#= 1 Atr=c0 MxPwr=  2mA
I:  If#= 0 Alt= 0 #EPs= 3 Cls=06(still) Sub=01 Prot=01 Driver=(none)
E:  Ad=01(O) Atr=02(Bulk) MxPS=  64 Ivl=0ms
E:  Ad=81(I) Atr=02(Bulk) MxPS=  64 Ivl=0ms
E:  Ad=82(I) Atr=03(Int.) MxPS=   8 Ivl=16ms

#endif

int mmptp::list_images(int extract)
{
	// A Kludge - For some reason, on the Kodak C875, if OPENSESSION is the first
	// operation, mmptp will lock up if invoked a second time to the same camera.
	unsigned char *device_info = NULL;
	int status = transaction(MMPTP_OPERATION_GETDEVICEINFO, -1, &device_info);
	if (status < 0)
		return status;

	int session_id = time(NULL);
	status = transaction(MMPTP_OPERATION_OPENSESSION, session_id);
	if (status < 0)
		return status;

	unsigned char *storage_ids = NULL;
	status = transaction(MMPTP_OPERATION_GETSTORAGEIDS, -1, &storage_ids);
	if ((status < 0) || !storage_ids)
		return status;
	unsigned char *storage_id_end = storage_ids + status;

	for (unsigned char *storage_id = storage_ids + 4; storage_id < storage_id_end; storage_id += 4)
		if ((char_to_int32(storage_id) & 0x00ff) == 0)
			printf("\nStorage ID %x\nNot Connected\n", char_to_int32(storage_id));

		else
		{
			printf("\nStorage ID %x ---------------\n", char_to_int32(storage_id));

			unsigned char *object_handles = NULL;
			status = transaction(MMPTP_OPERATION_GETOBJECTHANDLES, char_to_int32(storage_id), &object_handles);
			if (status < 0)
				return status;
			unsigned char *object_handle_end = object_handles + status;

			for (unsigned char *handle = object_handles + 4; handle < object_handle_end; handle += 4)
			{
				unsigned char *object_info = NULL;
				status = transaction(MMPTP_OPERATION_GETOBJECTINFO, char_to_int32(handle), &object_info);
				if (status < 0)
					return status;

				int format = char_to_int16(object_info + 4);
				char *protection = "rw-";
				if (char_to_int16(object_info + 6) == 1)
					protection = "r--";
				int size = char_to_int32(object_info + 8);
				int width = char_to_int32(object_info + 26);
				int height = char_to_int32(object_info + 30);
				int depth = char_to_int32(object_info + 34);
				unsigned char *filename = object_info + 52;
				unsigned char *capture_date = filename + 1 + (filename[0] * 2);
				unsigned char *modification_date = capture_date + 1 + (capture_date[0] * 2);
				unsigned char *keywords = modification_date + 1 + (modification_date[0] * 2);

				char unix_name[128] = "";
				time_t system_time = time(NULL);
				struct tm *local = localtime(&system_time);
				char *date = unicode_to_ascii(capture_date);
				if (date[0] == 0)
					date = unicode_to_ascii(modification_date);

				if (date[0] != 0)
					sprintf(unix_name, "%.4s-%.2s-%.2s_%.2s-%.2s-%.2s%s", 
						date, date + 4, date + 6, date + 9, date + 11, date + 13, 
						object_format_extension(format));
				else if (local)
				{
					strftime(unix_name, sizeof(unix_name), "%Y-%02m-%02d_%02H-%02m-", local);
					strcat(unix_name, unicode_to_ascii(filename));
				}
				else
					sprintf(unix_name, "%d-%s", (int) system_time, unicode_to_ascii(filename));

				printf(" %s %8d %s", protection, size, unix_name);
				printf(" %s", unicode_to_ascii(filename));
				//printf(" %s", unicode_to_ascii(capture_date));
				//printf(" %s", unicode_to_ascii(modification_date));
				printf(" %d x %d x %d", width, height, depth);
				printf(" %s\n", object_format_string(format));

				delete[] object_info;

				if (extract && (size > 1024))
				{
					unsigned char *object = NULL;
					status = transaction(MMPTP_OPERATION_GETOBJECT, char_to_int32(handle), &object, size);
					if (status < 0)
						return status;

					int descriptor = open(unix_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (descriptor < 0)
						perror(unix_name);
					else
					{
						write(descriptor, object, status);
						close(descriptor);
					}

					delete[] object;
				}
			}

			delete[] object_handles;
		}

	delete[] storage_ids;

	status = transaction(MMPTP_OPERATION_CLOSESESSION);

	release_camera();

	return status;
}

int mmptp::reset_camera()
{
	unsigned char *ignore = NULL;
	return transaction(MMPTP_OPERATION_RESETDEVICE);
}



/****************************** Command Line Interface ******************************/

int print_syntax()
{
	printf("SYNTAX: mmptp -i | -l | -d | -r\n");
	printf("        -l: list images\n");
	printf("        -d: download images to current directory\n");
	printf("        -i: print camera / storage info\n");
	printf("        -u: list all connected USB devices\n");
	// printf("        -r: reset camera (may not be supported)\n");
	return 0;
}


int main(int argc, char **argv)
{
	printf(VERSION_MESSAGE);
	mmptp ptp;

	if (argc != 2)
		return print_syntax();

	else if (strcmp(argv[1], "-u") == 0)
		return ptp.list_usb_devices();

	int status = ptp.acquire_camera();
	if (status < 0)
	{
		fprintf(stderr, "Failure opening camera: %s\n", strerror(-status));
		return status;
	}

	if (strcmp(argv[1], "-i") == 0)
		return ptp.print_device_info();

	else if (strcmp(argv[1], "-l") == 0)
		return ptp.list_images(0);

	else if (strcmp(argv[1], "-d") == 0)
		return ptp.list_images(1);

	//else if (strcmp(argv[1], "-r") == 0)
	//	return ptp.reset_camera();

	else
		return print_syntax();
}

/************
Commands are sent to USB device in containers.
Data is returned from USB device in containers.
Containers longer than maximum urb packet length are sent in multiple successive packets
Generic container structures - PIMA 15740:2000 spec, clause D 7.1.1, pp 148
/mnt/cdrom/Fedora/RPMS/libusb-devel-0.1.11-2.2.i386.rpm
*************/
