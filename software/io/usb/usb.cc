extern "C" {
	#include "itu.h"
    #include "dump_hex.h"
    #include "small_printf.h"
}
#include "usb.h"
#include "usb_device.h"

#include <stdlib.h>

BYTE c_clear_feature_stall[]   = { 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// =================
//   Configuration
// =================

#define CFG_USB_SPEED    0x51
#define CFG_USB_MAXCUR   0x52
#define CFG_USB_BOOT     0x53

char *usb_speed[] = { "Full Speed", "High Speed" };
char *usb_yesno[] = { "No", "Yes" };

struct t_cfg_definition usb_config[] = {
	{ CFG_USB_MAXCUR,   CFG_TYPE_VALUE,  "Maximum Bus Current",     "%d0 mA", NULL,       5, 35, 25 },
	{ CFG_USB_SPEED,    CFG_TYPE_ENUM,   "Bus Speed",                   "%s", usb_speed,  0,  1,  1 },
	{ CFG_USB_BOOT,     CFG_TYPE_ENUM,   "Load boot images from USB",   "%s", usb_yesno,  0,  1,  0 },
    { CFG_TYPE_END,     CFG_TYPE_END,    "", "", NULL, 0, 0, 0 }
};


__inline WORD le16_to_cpu(WORD h)
{
    return (h >> 8) | (h << 8);
}


void my_memcpy(void *a, void *b, int len)
{
    BYTE *dest = (BYTE *)a;
    BYTE *src = (BYTE *)b;
    while(len--) {
        *(dest++) = *(src++);
    }
}

#define memcpy my_memcpy

Usb usb; // the global

static void poll_usb(Event &e)
{
	usb.poll(e);
}

Usb :: Usb()
{
    debug = false;
	poll_delay = 0;
	initialized = false;
    
    if(CAPABILITIES & CAPAB_USB_HOST) {
	    clear();

#ifndef BOOTLOADER
        poll_list.append(&poll_usb);
        register_store(0x55534232, "USB settings", usb_config);
        if (cfg->get_value(CFG_USB_BOOT) ==0 )
            poll_delay = 6;
#endif
    }
}

Usb :: ~Usb()
{
#ifndef BOOTLOADER
	poll_list.remove(&poll_usb);
#endif
	clean_up();
	initialized = false;
}

void Usb :: print_status(void)
{
    int i, p;
    DWORD transact, pipe;

    const char *c_transaction_state_strings[] = { "None", "Busy", "Done", "Error" };
    const char *c_transaction_type_strings[] = { "Control", "Bulk", "Interrupt", "Isochronous" };
    const char *c_pipe_state_strings[] = { "None", "Initialized", "Stalled", "Invalid" };
    const char *c_pipe_direction_strings[] = { "In", "Out" };

    // show all non-zero transactions
    for(i=0;i<12;i++) {
        transact = USB_TRANSACTION(i);
        if(!transact)
            continue;
        p = (transact & 0x1F0) >> 4;
        printf("Transaction: %d\n", i);
        printf("   State: %s\n", c_transaction_state_strings[transact & 3]);
        printf("   Type: %s\n", c_transaction_type_strings[(transact & 0x0c)>>2]);
        printf("   Transfer Length: %d\n", ((transact & 0xFFE00) >> 9));
        printf("   Buffer Address: %d\n", ((transact & 0xFFF00000) >> 20));
        printf("   Pipe pointer: %d\n", p);
    }
    for(p=0;p<12;p++) {
        pipe = USB_PIPE(p);
        if(!pipe)
            continue;
        printf("Pipe: %d\n", p);
        printf("   State: %s\n", c_pipe_state_strings[pipe & 3]);
        printf("   Direction: %s\n", c_pipe_direction_strings[(pipe & 4) >> 2]);
        printf("   Device Addr: %d\n", (pipe & 0x3F8) >> 3);
        printf("   Endpoint: %d\n", (pipe & 0x3C00) >> 10);
        printf("   Max transfer: %d\n", (pipe & 0x1FFC000) >> 14);
        printf("   Toggle: %d\n", (pipe >> 25) & 1);
    }
}


UsbDevice *Usb :: first_device(void)
{
    return device_list[0];
}

void Usb :: clean_up()
{
    for(int i=0;i<USB_MAX_DEVICES;i++) {
        if(device_list[i]) {
        	device_list[i]->deinstall();
        	delete device_list[i]; // call destructor of device
        }
    }
}

void Usb :: clear()
{
    for(int i=0;i<USB_MAX_TRANSACTIONS;i++) {
        USB_TRANSACTION(i) = 0;
        transactions[i] = 0;
        callbacks[i] = 0;
        objects[i] = 0;
    }
    for(int i=0;i<USB_MAX_PIPES;i++) {
        USB_PIPE(i) = 0;
        pipes[i] = 0;
    }
    for(int i=0;i<USB_MAX_DEVICES;i++) {
        device_list[i] = NULL;
    }
    device_present = false;
	initialized = false;
}

int Usb :: get_device_slot(void)
{
    for(int i=0;i<USB_MAX_DEVICES;i++) {
        if(!device_list[i])
            return i;
    }
    return -1; // no empty slots
}

int  Usb :: write_ulpi_register(int addr, int value)
{
    if(USB_CMD_QUEUE_STAT & USB_CMD_QUEUE_FULL)
        return -1; 

    printf("Write reg %b with %b.\n", addr, value);
    USB_COMMAND = 0xC0 | (BYTE)addr;
    USB_COMMAND = (BYTE)value;
    return 0;
}
        
int  Usb :: read_ulpi_register(int addr)
{
    if(USB_CMD_QUEUE_STAT & USB_CMD_QUEUE_FULL)
        return -1; 

    USB_COMMAND = 0x80 | (BYTE)addr;
    while(!(USB_RESP_STATUS & USB_RESP_AVAILABLE))
        ;
    BYTE dummy = USB_RESP_GET;
    BYTE value = USB_RESP_DATA;
    BYTE stat  = USB_RESP_STATUS;

//    printf("%b %b %b\n", dummy, value, stat);
    if(stat & USB_RESP_REGVALUE)
        return (int)value;

    return -2;
}

int  Usb :: get_ulpi_status(void)
{
    if(USB_CMD_QUEUE_STAT & USB_CMD_QUEUE_FULL)
        return -1; 

    USB_COMMAND = USB_CMD_GET_STATUS;

    while(!(USB_RESP_STATUS & USB_RESP_AVAILABLE))
        ;
    BYTE dummy = USB_RESP_GET;
    BYTE value = USB_RESP_DATA;

    return (BYTE)value;    
}

int  Usb :: get_speed(void)
{
    if(USB_CMD_QUEUE_STAT & USB_CMD_QUEUE_FULL)
        return -1; 

    USB_COMMAND = USB_CMD_GET_SPEED;
    while(!(USB_RESP_STATUS & USB_RESP_AVAILABLE))
        ;
    BYTE dummy = USB_RESP_GET;
    BYTE value = USB_RESP_DATA;
    printf("Speed: $%b\n", value);
    
    return (int)value;
}
    
void Usb :: bus_reset(bool hs)
{
    // terminate all outstanding transactions
    USB_COMMAND = USB_CMD_DISABLE_HOST;
    wait_ms(2);
    for(int i=0;i<USB_MAX_TRANSACTIONS;i++) {
        USB_TRANSACTION(i) = 0;
    }
//    wait_ms(2);
//    write_ulpi_register(ULPI_FUNC_CTRL, FUNC_SUSPENDM | FUNC_RESET | FUNC_TERM_SEL | FUNC_XCVR_FS);
    wait_ms(2);
    write_ulpi_register(ULPI_FUNC_CTRL, FUNC_SUSPENDM | FUNC_RESET | FUNC_TERM_SEL | FUNC_XCVR_FS);
    wait_ms(100);
    if(hs)
        USB_COMMAND = USB_CMD_BUSRESET_HS;
    else
        USB_COMMAND = USB_CMD_BUSRESET_FS;
    
    wait_ms(100);
    speed = get_speed();
}
    
void Usb :: init(void)
{
    int s;

    // power down (temporary.. better to check if we are already powered)
    USB_COMMAND = USB_CMD_DISABLE_HOST;
	USB_COMMAND = USB_CMD_SOF_DISABLE;
    write_ulpi_register(ULPI_OTG_CONTROL, OTG_DP_PD | OTG_DM_PD | OTG_DISCHARGE);
    write_ulpi_register(ULPI_FUNC_CTRL, FUNC_SUSPENDM | FUNC_RESET | FUNC_TERM_SEL | FUNC_XCVR_FS);
    clean_up(); // delete all devices
    wait_ms(300);

    // power up
    clear(); // reinit structure
    s = read_ulpi_register(ULPI_IRQ_CURRENT);
    if(s & 2) {
        printf("*** There is already power on the bus!! *** Powered HUB?? ***\n");
        write_ulpi_register(ULPI_OTG_CONTROL, OTG_DP_PD | OTG_DM_PD);
    } else {
        write_ulpi_register(ULPI_OTG_CONTROL, OTG_DRV_VBUS | OTG_DP_PD | OTG_DM_PD );
        wait_ms(700);
    }
    
    // s = read_ulpi_register(ULPI_LINE_STATE);
    s = get_ulpi_status();
    printf("Linestatus before reset: $%b\n", s);
    if (!(s & 3)) {
        device_present = false;
        write_ulpi_register(ULPI_IRQEN_RISING, 0);
        read_ulpi_register(ULPI_IRQ_STATUS); // clear pending interrupts
        printf("No device present.\n");
    } else {
        device_present = true;
		USB_COMMAND = USB_CMD_SOF_ENABLE;
        wait_ms(30);
        bus_reset(cfg->get_value(CFG_USB_SPEED) > 0); // set to true for high speed
        wait_ms(30);
        write_ulpi_register(ULPI_IRQEN_RISING, IRQ_HOST_DISCONNECT);
        read_ulpi_register(ULPI_IRQ_STATUS); // clear pending interrupts
        attach_root();
    }
    initialized = true;
}
    
UsbDevice *Usb :: init_simple(void)
{
    int s;

    // power down (temporary.. better to check if we are already powered)
    USB_COMMAND = USB_CMD_DISABLE_HOST;
	USB_COMMAND = USB_CMD_SOF_DISABLE;
    write_ulpi_register(ULPI_OTG_CONTROL, OTG_DP_PD | OTG_DM_PD | OTG_DISCHARGE);
    write_ulpi_register(ULPI_FUNC_CTRL, FUNC_SUSPENDM | FUNC_RESET | FUNC_TERM_SEL | FUNC_XCVR_FS);
    wait_ms(300);

    // power up
    clear(); // reinit structure
    write_ulpi_register(ULPI_OTG_CONTROL, OTG_DRV_VBUS | OTG_DP_PD | OTG_DM_PD );
    wait_ms(700);

    // s = read_ulpi_register(ULPI_LINE_STATE);
    s = get_ulpi_status();
    if (!(s & 3)) {
        printf("No device present.\n");
    	return NULL;
    }
	initialized = true;
	USB_COMMAND = USB_CMD_SOF_ENABLE;
	bus_reset(false); // set to true for high speed
	wait_ms(30);
	UsbDevice *dev = new UsbDevice(this);
	if(dev) {
		if(!dev->init(1)) {
			delete dev;
			return NULL;
		}
	}
	return dev;
}

void Usb :: attach_root(void)
{
    printf("Attach root!!\n");
    max_current = cfg->get_value(CFG_USB_MAXCUR) * 10;
    remaining_current = max_current;

    int s = get_ulpi_status();
    printf("Status on attach attempt: $%b\n", s);

	if((s & STAT_VBUS_BITS)!=STAT_VBUS_VLD) {
		printf("USB Bus Voltage low..\n");
		return;
	}
/*	    
    if(i != 0) {
        printf("For some reason i am not getting slot 0.. \n Internal error\n");
        return;
    }
*/
    UsbDevice *dev = new UsbDevice(this);
	install_device(dev, true);
}

bool Usb :: install_device(UsbDevice *dev, bool draws_current)
{
    int idx = get_device_slot();
    
//    if(!dev->init(idx+1)) {
//    	printf("Failed to init USB device. Not installed.\n");
//    	return false;
//    }
    bool ok = false;
    for(int i=0;i<20;i++) { // try 20 times!
        if(dev->init(idx+1)) {
            ok = true;
            break;
        }
    }
    if(!ok)
        return false;

    if(!draws_current) {
        device_list[idx] = dev;
        dev->install();
        return true; // actually install should be able to return false too
    } else {
        int device_curr = int(dev->device_config.max_power) * 2;

        if(device_curr > remaining_current) {
        	printf("Device current (%d mA) exceeds maximum remaining current (%d mA).\n", device_curr, remaining_current);
//            delete dev;
//            return false;
        }
//        else {
        device_list[idx] = dev;
        remaining_current -= device_curr;
        dev->install();
        return true; // actually install should be able to return false too
//        }
    }
    return false;
}

void Usb :: deinstall_device(UsbDevice *dev)
{
    dev->deinstall();
    for(int i=0;i<USB_MAX_DEVICES;i++) {
        if(device_list[i] == dev) {
        	device_list[i] = NULL;
        	delete dev;
        	break;
        }
    }
}
    
void Usb :: poll(Event &e)
{
	if(poll_delay > 0) {
		--poll_delay;
		return;
	}

	if(!initialized)
		return init();

	int irq = 0;
	if(device_present) {
		bool disconnect = false;
		if(speed == 2) {
            //read_ulpi_register(ULPI_VENDOR_ID_L);
        	irq = read_ulpi_register(ULPI_IRQ_STATUS); // clear pending interrupts
        	if(irq & IRQ_HOST_DISCONNECT)
        		disconnect = true;
        } else {
	        int s = get_ulpi_status();
	        if((s & 3) == 0) {
	        	se0_seen ++;
	        	if(se0_seen == 5)
	        		disconnect = true;
	        } else {
	        	se0_seen = 0;
	        }
		}
		if(disconnect) {
            printf("Disconnected %b!\n", irq);
            device_present = false;
            clean_up(); // since the root was removed, all devices need to be disabled
            USB_COMMAND = USB_CMD_DISABLE_HOST;
            write_ulpi_register(ULPI_OTG_CONTROL, 0x0e);
            // default back to FS.
            write_ulpi_register(ULPI_FUNC_CTRL, FUNC_SUSPENDM | FUNC_RESET | FUNC_TERM_SEL | FUNC_XCVR_FS);
            speed = 1;
            read_ulpi_register(ULPI_IRQ_STATUS); // clear pending interrupts

            clear(); // we also prepare for re-insertion
            poll_delay = 10;
        } else {
			UsbDevice *dev;
			UsbDriver *drv;

			for(int i=5;i<USB_MAX_TRANSACTIONS;i++) {
				if (!transactions[i])
					continue;
				interrupt_in(i, input_pipe_numbers[i], 8); // may call callbacks
			}
			for(int i=0;i<USB_MAX_DEVICES;i++) {
				dev = device_list[i];
				if(dev) {
					drv = dev->driver;
					if(drv) {
						drv->poll();
					}
				}
			}
        }
    } else { // insert?
        int s = get_ulpi_status();
        if(s & 3) {
            printf("Connected!\n");
            device_present = true;
    		USB_COMMAND = USB_CMD_SOF_ENABLE;
            bus_reset(cfg->get_value(CFG_USB_SPEED) > 0); // set to true for high speed
            wait_ms(30);
            if(speed == 2) { // this is the method to see if a high speed device is disconnected
				write_ulpi_register(ULPI_IRQEN_RISING, IRQ_HOST_DISCONNECT);
				read_ulpi_register(ULPI_IRQ_STATUS); // clear pending interrupts
				write_ulpi_register(ULPI_IRQEN_RISING, IRQ_HOST_DISCONNECT);
            }
            attach_root();
        }
    }
}

int Usb :: create_pipe(int dev_addr, struct t_endpoint_descriptor *epd)
{
    DWORD pipe = 0L;
    
    // find free pipe
    int index = -1;
    for(int i=0;i<USB_MAX_PIPES;i++) {
        if(!pipes[i]) {
            index = i;
            break;
        }
    }
    if(index == -1)
        return -1;

    pipe = ((DWORD)(epd->endpoint_address & 0x0f)) << 10;
    if(!(epd->endpoint_address & 0x80)) { // out?
        pipe |= 4L;
    }
    pipe |= ((DWORD)dev_addr) << 3;
    pipe |= ((DWORD)le16_to_cpu(epd->max_packet_size)) << 14;
    pipe |= 1; //initialized
    // now allocate pipe in usb structure
    USB_PIPE(index+2) = pipe;
    pipes[index] = pipe;
    printf("Pipe with value %8x created. Max Packet = %d. Index = %d. Dir = %s\n", pipe, le16_to_cpu(epd->max_packet_size), index+2, (pipe & 4)?"out":"in");
    return index+2; // value to be used in transactions
}

void Usb :: free_pipe(int index)
{
    if(index >= (USB_MAX_PIPES+2))
        return;
    if(index < 2)
        return;
    pipes[index-2] = 0;
}

int Usb :: control_exchange(int addr, void *out, int outlen, void *in, int inlen, BYTE **buf)
{
    memcpy((void *)&USB_BUFFER(0), out, outlen);

    // TODO: just FORCE the coming two transactions to be at the beginning of a frame, and always one after another
    // Putting the setup and in transactions too fast after one another cause some devices to get into an undefined state
    // so it wasn't so bad to just wait for the first to finish, and then start the next!

    USB_COMMAND = USB_CMD_SCAN_DISABLE;
    wait_ms(1);

    DWORD a = ((DWORD)addr)<<3;
	USB_PIPE(0) = 0x04100005 | a; // Dev address a, Endpoint 0, Control Out
	USB_PIPE(1) = 0x04100001 | a; // Dev address a, Endpoint 0, Control In

	volatile DWORD *transaction;
	
//    USB_TRANSACTION(0) = 0x80000001 | (outlen << 9); // outlen bytes to pipe 0, from buffer 0x000, LINK TO NEXT
//	USB_TRANSACTION(1) = 0x82000015 | (inlen << 9);  // inlen bytes from pipe 1, to buffer 0x020, LINK TO NEXT
//    USB_TRANSACTION(2) = 0x00800005;                // outlen bytes to pipe 0, zero bytes
    
    USB_TRANSACTION(0) = 0x00000001 | (outlen << 9); // outlen bytes to pipe 0, from buffer 0x000, LINK TO NEXT
	transaction = &USB_TRANSACTION(0);

    USB_COMMAND = USB_CMD_SCAN_ENABLE; // go!
//    if(debug) {
//        USB_COMMAND = USB_CMD_SET_DEBUG;
//        debug = false;
//    }
    
    int timeout = 100; // 25 ms
	while((*transaction & 0x3) == 0x01) {
        if(!timeout) {
            // clear
            USB_TRANSACTION(0) = 0;
            USB_TRANSACTION(1) = 0;
            USB_TRANSACTION(2) = 0;
			USB_COMMAND = USB_CMD_ABORT;
			wait_ms(1);
            return -1;
        }
        timeout --;
        ITU_TIMER = 50;
        while(ITU_TIMER)
            ;
    }

    if((*transaction & 0x3) == 0x03) { // error
        printf("ERROR SETUP: pipe status = %8x\n", USB_PIPE(0));
        return -2;
    }

    if(!inlen)
        return 0;
        
	USB_TRANSACTION(1) = 0x02000015 | (inlen << 9);  // inlen bytes from pipe 1, to buffer 0x020, LINK TO NEXT
	transaction = &USB_TRANSACTION(1);

    timeout = 100; // 25 ms
	while((*transaction & 0x3) == 0x01) {
        if(!timeout) {
            // clear
            USB_TRANSACTION(0) = 0;
            USB_TRANSACTION(1) = 0;
            USB_TRANSACTION(2) = 0;
			USB_COMMAND = USB_CMD_ABORT;
			wait_ms(1);
            return -1;
        }
        timeout --;
        ITU_TIMER = 50;
        while(ITU_TIMER)
            ;
    }

    if((*transaction & 0x3) == 0x03) { // error
        printf("ERROR CTRL IN: pipe status = %8x\n", USB_PIPE(1));
        return -3;
    }

	DWORD ta = *transaction;
    int received = inlen - ((ta >> 9) & 0x7FF);
	//printf("Transaction: %8x InLen=%d Received=%d\n", ta, inlen, received);
    if(received < 0) {
        dump_hex((void *)&USB_BUFFER(32), 64);
    }
    if(received < 0)
        return -1;

    if(in) // user requested copy
        memcpy(in, (void *)&USB_BUFFER(32), received);

    if(buf) // user requested pointer to usb buffer
        *buf = (BYTE *)&USB_BUFFER(32);
        
    return received;
}

int Usb :: control_write(int addr, void *setup_out, int setup_len, void *data_out, int data_len)
{
    memcpy((void *)&USB_BUFFER(0), setup_out, setup_len);

    // TODO: just FORCE the coming two transactions to be at the beginning of a frame, and always one after another
    // Putting the setup and in transactions too fast after one another cause some devices to get into an undefined state
    // so it wasn't so bad to just wait for the first to finish, and then start the next!

    USB_COMMAND = USB_CMD_SCAN_DISABLE;
    wait_ms(1);

    DWORD a = ((DWORD)addr)<<3;
	USB_PIPE(0) = 0x04100005 | a; // Dev address a, Endpoint 0, Control Out
	USB_PIPE(1) = 0x02100005 | a; // Dev address a, Endpoint 0, Generic Out, Toggle-bit=1

	volatile DWORD *transaction;
	
    USB_TRANSACTION(0) = 0x00000001 | (setup_len << 9); // setup_len bytes to pipe 0, from buffer 0x000, LINK TO NEXT
	transaction = &USB_TRANSACTION(0);

    USB_COMMAND = USB_CMD_SCAN_ENABLE; // go!
    if(debug) {
        USB_COMMAND = USB_CMD_SET_DEBUG;
        debug = false;
    }
    
    int timeout = 100; // 25 ms
	while((*transaction & 0x3) == 0x01) {
        if(!timeout) {
            // clear
            USB_TRANSACTION(0) = 0;
            USB_TRANSACTION(1) = 0;
            USB_TRANSACTION(2) = 0;
			USB_COMMAND = USB_CMD_ABORT;
			wait_ms(1);
            return -1;
        }
        timeout --;
        ITU_TIMER = 50;
        while(ITU_TIMER)
            ;
    }

    if((*transaction & 0x3) == 0x03) { // error
        printf("ERROR SETUP: pipe status = %8x\n", USB_PIPE(0));
        return -2;
    }

    if(!data_len)
        return 0;
        
    memcpy((void *)&USB_BUFFER(0x20), data_out, data_len);
    //dump_hex((void *)&USB_BUFFER(0), 64);
	USB_TRANSACTION(1) = 0x02000015 | (data_len << 9);  // data_len bytes to pipe 1, from buffer 0x020
	transaction = &USB_TRANSACTION(1);

    timeout = 100; // 25 ms
	while((*transaction & 0x3) == 0x01) {
        if(!timeout) {
            // clear
            USB_TRANSACTION(0) = 0;
            USB_TRANSACTION(1) = 0;
			USB_COMMAND = USB_CMD_ABORT;
			wait_ms(1);
            return -1;
        }
        timeout --;
        ITU_TIMER = 50;
        while(ITU_TIMER)
            ;
    }

    if((*transaction & 0x3) == 0x03) { // error
        printf("ERROR CTRL OUT: pipe status = %8x\n", USB_PIPE(1));
        return -3;
    }

	DWORD ta = *transaction;
    int transmitted = data_len - ((ta >> 9) & 0x7FF);
	//printf("Transaction: %8x DataLen=%d Transmitted=%d\n", ta, data_len, transmitted);
    if(transmitted < 0)
        return -1;

    return transmitted;
}

void Usb :: unstall_pipe(int pipe)
{
    DWORD p = USB_PIPE(pipe);
    int addr = (p >> 3) & 0x7F;
    int endp = ((p >> 10) & 0x0F);

    if((p&3)==2) {
    	printf("STALLED: ");
    } else {
    	printf("Not stalled: ");
	}
	printf("Unstalling pipe. Addr = %d. EP = %b\n", addr, endp);
	c_clear_feature_stall[4] = (BYTE)endp;
	control_exchange(addr, c_clear_feature_stall, 8, NULL, 8, NULL);
	p = (p & -4) | 1;
	USB_PIPE(pipe) = p;
}

int Usb :: bulk_out_with_prefix(void *prefix, int prefix_len, void *buf, int len, int pipe)
{
    memcpy((void *)&USB_BUFFER(256), prefix, prefix_len);
    memcpy((void *)&USB_BUFFER(256+prefix_len), buf, len);
    return bulk_out_actual(prefix_len + len, pipe);
}

/*
BYTE *Usb :: get_bulk_out_buffer(int pipe)
{
    return (BYTE *)&USB_BUFFER(256);
}
*/    

int Usb :: bulk_out(void *buf, int len, int pipe)
{
    int total = 0;
    char *b = (char *)buf;
    while(len > 0) {
        int len_now = (len > 512)?512:len;
        memcpy((void *)&USB_BUFFER(256), b, len_now);
        int res = bulk_out_actual(len_now, pipe);
        if (res <= 0)
            return total;
        b += res;
        len -= res;            
        total += res;
    }
    return total;
}
    
int Usb :: bulk_out_actual(int len, int pipe)
{

//    printf("Pipe = %08x\n", USB_PIPE(pipe));
//    USB_PIPE(pipe) ^= 0x02000000; // toggle data (debug)

    // clear pipe's stall status, if any
    DWORD p = USB_PIPE(pipe);
/*
    if((p&3)==2) {
        int addr = (p >> 3) & 0x7F;
        int endp = ((p >> 10) & 0x0F);
        printf("Unstalling OUT pipe. Addr = %d. EP = %b\n", addr, endp);
        c_clear_feature_stall[4] = (BYTE)endp;
        control_exchange(addr, c_clear_feature_stall, 8, NULL, 8, NULL);
        p = (p & -4) | 1;
        USB_PIPE(pipe) = p;
    }
*/
	// remove abort condition
	if((p&3)==3) {
        p = (p & -4) | 1;
        USB_PIPE(pipe) = p;
	}
/*
    printf("Bulk out: ");
    for(int i=0;i<len;i++) 
        printf("%b ", ((BYTE *)buf)[i]);
    printf(".\n");
*/
    volatile DWORD *transaction = &USB_TRANSACTION(4);
    USB_TRANSACTION(4) = 0x10000005 | (DWORD)(len << 9) | (DWORD)(pipe << 4);
    
    int timeout = 5000;
    while((*transaction & 0x3) == 0x01) {
        if(!timeout) {
        	printf("Timeout. Transaction = %8x %d\n", *transaction, get_ulpi_status());
        	USB_TRANSACTION(4) = 0;
        	USB_COMMAND = USB_CMD_ABORT;
            wait_ms(1);
            return -1;
        }
        timeout --;
        wait_ms(1);
    }
    if((*transaction & 0x3) == 0x03) { // error
        printf("ERROR OUT: pipe status = %8x\n", USB_PIPE(pipe));
        return 0;
    }
    
    return len;
}

int Usb :: bulk_in(void *buf, int len, int pipe)
{
    int new_len;
    BYTE *buffer = (BYTE*)buf;
    
	// clear pipe's stall status, if any
    DWORD p = USB_PIPE(pipe);

    if((p&3)==2) {
    	printf("*** WARNING IN-PIPE IS STALLED ***\n");

    	int addr = (p >> 3) & 0x7F;
        int endp = ((p >> 10) & 0x0F) | 0x80;
        printf("Unstalling IN pipe. Addr = %d. EP = %b\n", addr, endp);
        c_clear_feature_stall[4] = (BYTE)endp;
        control_exchange(addr, c_clear_feature_stall, 8, NULL, 8, NULL);
        p = (p & -4) | 1;
        USB_PIPE(pipe) = p;
    }

	// remove abort condition
	if((p&3)==3) {
        p = (p & -4) | 1;
        USB_PIPE(pipe) = p;
	}

    volatile DWORD *transaction = &USB_TRANSACTION(3);

    int current;
    int transferred = 0;
    
    while(len > 0) {
        current = len;
        if(current > 512)  // or max pipe, but hardware should already split into small chunks
            current = 512;

        USB_TRANSACTION(3) = 0x30000005 | (DWORD)(current << 9) | (DWORD)(pipe << 4);
//        USB_TRANSACTION(3) = 0x30000005 | (DWORD)(len << 9) | (DWORD)(pipe << 4);
        
        int timeout = 5000; 
        while((*transaction & 0x3) == 0x01) {
            if(!timeout) {
            	USB_TRANSACTION(3) = 0;
                USB_COMMAND = USB_CMD_ABORT;
                wait_ms(1);
                return -1;
            }
            timeout --;
            wait_ms(1);
        }
    
        if((*transaction & 0x3) == 0x03) { // error
            printf("ERROR IN: pipe status = %8x\n", USB_PIPE(pipe));
            return 0;
        }

        new_len = (int)((USB_TRANSACTION(3) >> 9) & 0x7FF);
        new_len = current - new_len;
//        if(new_len < 0)
//            new_len = 0;
    
    //    printf("len = %d. new len = %d\n", len, new_len);
        if(!new_len)
            printf("No data. Pipe: %8x\n", USB_PIPE(pipe));
    
        memcpy(buffer, (void *)&USB_BUFFER(768), new_len);
        buffer += new_len;
        len -= current;
        transferred += new_len;
    }
    return transferred;
}


int Usb :: allocate_input_pipe(int len, int pipe, void(*callback)(BYTE *buf, int len, void *obj), void *object)
{
    // find free transaction
    int index = -1;
    for(int i=5;i<USB_MAX_TRANSACTIONS;i++) {
        if(!transactions[i]) {
            index = i;
            break;
        }
    }
    if(index == -1)
        return -1;

    if(len > 8) // bigger sizes not yet supported as we need an allocator or DMA mode
        return -1;
        
    int offset = 1280 + (8 * index);
    transactions[index] = offset;
    
    printf("Allocated transaction %d at $%3x.\n", index, offset);

    callbacks[index] = callback;
    objects[index] = object;
    input_pipe_numbers[index] = pipe;

    return index;
}

void Usb :: free_input_pipe(int index)
{
    if(index < 5)
        return;

    printf("Freeing transaction %d.\n", index);
    if(transactions[index]) {
        transactions[index] = 0;
        USB_TRANSACTION(index) = 0L;
    }
}

int Usb :: interrupt_in(int trans, int pipe, int len)
{
    int offset = transactions[trans];
    volatile DWORD *tr = &USB_TRANSACTION(trans);
    void (*callback)(BYTE *buf, int len, void *obj);

    DWORD readback_status = (*tr) & 3;
    
    // first the good case.
    if (readback_status == 2) { // done
        // memcpy(buf, (void *)&USB_BUFFER(offset), len);
        callback = callbacks[trans];
        if(callback) {
        	callback((BYTE *)&USB_BUFFER(offset), len, objects[trans]);
        } else {
        	printf("Callback pointer is null! trans = %d\n", trans);
        }
        DWORD opcode = (offset << 20) | (len << 9) | (pipe << 4) | 0x09; // busy | interrupt
        *tr = opcode; // restart
        return len;
    }
    
    if(readback_status == 3) {
        printf("Interrupt error.. %8x (pipe = %8x)\n", readback_status, USB_PIPE(pipe));
        *tr = 0; // fake none
    } else if (readback_status == 0) { // none
        DWORD opcode = (offset << 20) | (len << 9) | (pipe << 4) | 0x09; // busy | interrupt
        printf("No interrupt active on transaction %d (%8x), writing %8x\n", trans, readback_status, opcode);
        *tr = opcode; // start
        return 0;
    } 
    return 0;    
}
/*
// FIXME!! This function can only be used by one pipe, since we use static allocation of our buffer!
int Usb :: start_bulk_in(int trans, int pipe, int len)
{
    volatile DWORD *tr = &USB_TRANSACTION(trans);
    int offset = 0x600;
    DWORD opcode = (offset << 20) | (len << 9) | (pipe << 4) | 0x05; // busy | bulk
    *tr = opcode;
}

int Usb :: transaction_done(int trans)
{
    volatile DWORD *tr = &USB_TRANSACTION(trans);
    DWORD readback_status = (*tr) & 3;
    if (readback_status == 2)
        return 1;
    if (readback_status == 3)
        return -1;
    return 0;
}

int Usb :: get_bulk_in_data(int trans, BYTE **buf)
{
    volatile DWORD *tr = &USB_TRANSACTION(trans);
    int datalen = 512 - (int)((*tr >> 9) & 0x7FF);
    if(datalen < 0)
        datalen = 0;
    *buf = (BYTE *)&USB_BUFFER(0x600);
    return datalen;
}
*/
