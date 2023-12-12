/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#define CLIENT_NUM_EVENT_MSG        5


// CDC devices often implement Interface Association Descriptor (IAD). Parse IAD only when
// bDeviceClass = 0xEF (Miscellaneous Device Class), bDeviceSubClass = 0x02 (Common Class), bDeviceProtocol = 0x01 (Interface Association Descriptor),
// or when bDeviceClass, bDeviceSubClass, and bDeviceProtocol are 0x00 (Null class code triple), as per https://www.usb.org/defined-class-codes, "Base Class 00h (Device)" section
// @see USB Interface Association Descriptor: Device Class Code and Use Model rev 1.0, Table 1-1
#define USB_SUBCLASS_NULL        0x00
#define USB_SUBCLASS_COMMON        0x02
#define USB_PROTOCOL_NULL    0x00
#define USB_DEVICE_PROTOCOL_IAD    0x01

typedef enum {
    ACTION_OPEN_DEV = 0x01,
    ACTION_GET_DEV_INFO = 0x02,
    ACTION_GET_DEV_DESC = 0x04,
    ACTION_GET_CONFIG_DESC = 0x08,
    ACTION_GET_STR_DESC = 0x10,
    ACTION_CLOSE_DEV = 0x20,
    ACTION_EXIT = 0x40,
    ACTION_RECONNECT = 0x80,
} action_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint32_t actions;
} class_driver_t;

static const char *TAG = "CLASS";
static class_driver_t *s_driver_obj;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (driver_obj->dev_addr == 0) {
            driver_obj->dev_addr = event_msg->new_dev.address;
            //Open the device next
            driver_obj->actions |= ACTION_OPEN_DEV;
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (driver_obj->dev_hdl != NULL) {
            //Cancel any other actions and close the device next
            driver_obj->actions = ACTION_CLOSE_DEV;
        }
        break;
    default:
        //Should never occur
        abort();
    }
}

static void action_open_dev(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_addr != 0);
    ESP_LOGI(TAG, "Opening device at address %d", driver_obj->dev_addr);
    ESP_ERROR_CHECK(usb_host_device_open(driver_obj->client_hdl, driver_obj->dev_addr, &driver_obj->dev_hdl));
    //Get the device's information next
    driver_obj->actions &= ~ACTION_OPEN_DEV;
    driver_obj->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device information");
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    ESP_LOGI(TAG, "\t%s speed", (dev_info.speed == USB_SPEED_LOW) ? "Low" : "Full");
    ESP_LOGI(TAG, "\tbConfigurationValue %d", dev_info.bConfigurationValue);

    //Get the device descriptor next
    driver_obj->actions &= ~ACTION_GET_DEV_INFO;
    driver_obj->actions |= ACTION_GET_DEV_DESC;
}

static void action_get_dev_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device descriptor");
    const usb_config_desc_t *config_desc;
    const usb_device_desc_t *dev_desc;
    int desc_offset = 0;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(driver_obj->dev_hdl, &dev_desc));
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));
    usb_print_device_descriptor(dev_desc);
    if (((dev_desc->bDeviceClass == USB_CLASS_MISC) && (dev_desc->bDeviceSubClass == USB_SUBCLASS_COMMON) &&
            (dev_desc->bDeviceProtocol == USB_DEVICE_PROTOCOL_IAD)) ||
            ((dev_desc->bDeviceClass == USB_CLASS_PER_INTERFACE) && (dev_desc->bDeviceSubClass == USB_SUBCLASS_NULL) &&
             (dev_desc->bDeviceProtocol == USB_PROTOCOL_NULL))) {
        // This is a composite device, that uses Interface Association Descriptor
    	printf("This is a composite device, that uses Interface Association Descriptor \n");
    	const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)config_desc;
        do {
            this_desc = usb_parse_next_descriptor_of_type(
                            this_desc, config_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION, &desc_offset);

            if (this_desc == NULL) {
                break;    // Reached end of configuration descriptor
            }

            const usb_iad_desc_t *iad_desc = (const usb_iad_desc_t *)this_desc;
                // IAD with correct interface number was found: Check Class/Subclass codes, save Interface indexes
                printf("iad_desc->bFirstInterface   %i \n", iad_desc->bFirstInterface);
                printf("iad_desc->bInterfaceCount   %i \n", iad_desc->bInterfaceCount);
                printf("iad_desc->bFunctionClass    %i \n", iad_desc->bFunctionClass);
                printf("iad_desc->bFunctionSubClass %i \n", iad_desc->bFunctionSubClass);
        } while (1);
    } else if ((dev_desc->bDeviceClass == USB_CLASS_COMM)) {
        // This is a Communication Device Class
    	printf("This is a Communication Device Class \n");
    }






    //Get the device's config descriptor next
    driver_obj->actions &= ~ACTION_GET_DEV_DESC;
    driver_obj->actions |= ACTION_GET_CONFIG_DESC;
}

static void print_ep_desc(const usb_ep_desc_t *ep_desc)
{
    const char *ep_type_str;
    int type = ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;

    switch (type) {
    case USB_BM_ATTRIBUTES_XFER_CONTROL:
        ep_type_str = "CTRL";
        break;
    case USB_BM_ATTRIBUTES_XFER_ISOC:
        ep_type_str = "ISOC";
        break;
    case USB_BM_ATTRIBUTES_XFER_BULK:
        ep_type_str = "BULK";
        break;
    case USB_BM_ATTRIBUTES_XFER_INT:
        ep_type_str = "INT";
        break;
    default:
        ep_type_str = NULL;
        break;
    }

    printf("\t\t*** Endpoint descriptor ***\n");
    printf("\t\tbLength %d\n", ep_desc->bLength);
    printf("\t\tbDescriptorType %d\n", ep_desc->bDescriptorType);
    printf("\t\tbEndpointAddress 0x%x\tEP %d %s\n", ep_desc->bEndpointAddress,
           USB_EP_DESC_GET_EP_NUM(ep_desc),
           USB_EP_DESC_GET_EP_DIR(ep_desc) ? "IN" : "OUT");
    if (USB_EP_DESC_GET_XFERTYPE(ep_desc) == USB_TRANSFER_TYPE_INTR) {
        // Notification channel does not have its dedicated interface (data and notif interface is the same)
        printf("\t\t NOTIF \n");
    } else if (USB_EP_DESC_GET_XFERTYPE(ep_desc) == USB_TRANSFER_TYPE_BULK) {
        if (USB_EP_DESC_GET_EP_DIR(ep_desc)) {
        	printf("\t\t IN \n");
        } else {
        	printf("\t\t OUT \n");
        }
    }
    printf("\t\tbmAttributes 0x%x\t%s\n", ep_desc->bmAttributes, ep_type_str);
    printf("\t\twMaxPacketSize %d\n", ep_desc->wMaxPacketSize);
    printf("\t\tbInterval %d\n", ep_desc->bInterval);
}


static void usbh_print_intf_desc(const usb_intf_desc_t *intf_desc)
{
    printf("\t*** Interface descriptor ***\n");
    printf("\tbLength %d\n", intf_desc->bLength);
    printf("\tbDescriptorType %d\n", intf_desc->bDescriptorType);
    printf("\tbInterfaceNumber %d\n", intf_desc->bInterfaceNumber);
    printf("\tbAlternateSetting %d\n", intf_desc->bAlternateSetting);
    printf("\tbNumEndpoints %d\n", intf_desc->bNumEndpoints);
    printf("\tbInterfaceClass 0x%x\n", intf_desc->bInterfaceClass);
    printf("\tbInterfaceSubClass 0x%x\n", intf_desc->bInterfaceSubClass);
    printf("\tbInterfaceProtocol 0x%x\n", intf_desc->bInterfaceProtocol);
    printf("\tiInterface %d\n", intf_desc->iInterface);
}

static void action_get_config_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting config descriptor");
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));

    printf("\t*** ******************** config descriptor begin ******************* ***\n");
    usb_print_config_descriptor(config_desc, NULL);
    printf("\t*** ******************** config descriptor end ******************* ***\n");

    int j = 0;
    int desc_offset = 0;
    for (j = 0; j <= 50; ++j) {

        const usb_intf_desc_t * intf_desc = usb_parse_interface_descriptor(config_desc, j, 0, &desc_offset);
        if(intf_desc == NULL) break;
        const int temp_offset = desc_offset; // Save this offset for later

        printf("\t*** ******************** Interface descriptor begin ******************* ***\n");
        usbh_print_intf_desc(intf_desc);
        printf("\t*** ******************** Interface descriptor end ******************* ***\n");

        // Go through all interface's endpoints and parse Interrupt and Bulk endpoints
        for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
            const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &desc_offset);
            assert(this_ep);
            printf("found Endpoint: %i on Interface %i ", i, j);
            if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_INTR) {
                // Notification channel does not have its dedicated interface (data and notif interface is the same)
                printf("this is a NOTIF/DATA EP \n");
            } else if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_BULK) {
                if (USB_EP_DESC_GET_EP_DIR(this_ep)) {
                    printf("this is a IN EP \n");
                } else {
                    printf("this is a OUT EP \n");
                }
            }
            printf("\t*** ******************** Endpoint descriptor  begin ******************* ***\n");
            print_ep_desc(this_ep);
            printf("\t*** ******************** Endpoint descriptor  end ******************* ***\n");
            desc_offset = temp_offset;
        }
	}


    //Get the device's string descriptors next
    driver_obj->actions &= ~ACTION_GET_CONFIG_DESC;
    driver_obj->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(class_driver_t *driver_obj)
{
    assert(driver_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    if (dev_info.str_desc_manufacturer) {
        ESP_LOGI(TAG, "Getting Manufacturer string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_manufacturer);
    }
    if (dev_info.str_desc_product) {
        ESP_LOGI(TAG, "Getting Product string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_product);
    }
    if (dev_info.str_desc_serial_num) {
        ESP_LOGI(TAG, "Getting Serial Number string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_serial_num);
    }
    //Nothing to do until the device disconnects
    driver_obj->actions &= ~ACTION_GET_STR_DESC;
}

static void action_close_dev(class_driver_t *driver_obj)
{
    ESP_ERROR_CHECK(usb_host_device_close(driver_obj->client_hdl, driver_obj->dev_hdl));
    driver_obj->dev_hdl = NULL;
    driver_obj->dev_addr = 0;
    //We need to connect a new device
    driver_obj->actions &= ~ACTION_CLOSE_DEV;
    driver_obj->actions |= ACTION_RECONNECT;
}

void class_driver_task(void *arg)
{
    class_driver_t driver_obj = {0};

    ESP_LOGI(TAG, "Registering Client");
    usb_host_client_config_t client_config = {
        .is_synchronous = false,    //Synchronous clients currently not supported. Set this to false
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = (void *) &driver_obj,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &driver_obj.client_hdl));
    s_driver_obj = &driver_obj;

    while (1) {
        if (driver_obj.actions == 0) {
            usb_host_client_handle_events(driver_obj.client_hdl, portMAX_DELAY);
        } else {
            if (driver_obj.actions & ACTION_OPEN_DEV) {
                action_open_dev(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_INFO) {
                action_get_info(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_DEV_DESC) {
                action_get_dev_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_CONFIG_DESC) {
                action_get_config_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_GET_STR_DESC) {
                action_get_str_desc(&driver_obj);
            }
            if (driver_obj.actions & ACTION_CLOSE_DEV) {
                action_close_dev(&driver_obj);
            }
            if (driver_obj.actions & ACTION_EXIT) {
                break;
            }
            if (driver_obj.actions & ACTION_RECONNECT) {
                driver_obj.actions = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Deregistering Client");
    ESP_ERROR_CHECK(usb_host_client_deregister(driver_obj.client_hdl));
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    if (s_driver_obj->dev_hdl != NULL) {
        s_driver_obj->actions = ACTION_CLOSE_DEV;
    }
    s_driver_obj->actions |= ACTION_EXIT;

    // Unblock, exit the loop and proceed to deregister client
    ESP_ERROR_CHECK(usb_host_client_unblock(s_driver_obj->client_hdl));
}
