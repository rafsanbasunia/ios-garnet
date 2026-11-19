/*
 * The registry entry by which Darwin's Bluetooth daemon finds the controller's address.
 *
 * bluetoothd looks for a service named "bluetooth", which on an iPhone is a node of the device tree, and reads
 * its property local-mac-address; without one it ends. The controller here is on a UART of the SoC
 * (drivers/bluetooth/hakios.btqca.c), which says its address when it is up: that is published here.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>

extern "C" void hakios_bt_publish(const uint8_t * address);

void
hakios_bt_publish(const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	IOService * node = OSTypeAlloc(IOService);
	OSData * mac;

	if (platform == NULL || node == NULL || !node->init()) {
		OSSafeReleaseNULL(node);
		IOLog("bt: the registry entry \"bluetooth\" cannot be made\n");
		return;
	}
	if ((mac = OSData::withBytes(address, 6)) != NULL) {
		node->setProperty("local-mac-address", mac);
		mac->release();
	}
	if (!node->attach(platform)) {
		node->release();
		IOLog("bt: the registry entry \"bluetooth\" cannot be attached\n");
		return;
	}
	node->setName("bluetooth");
	if (!node->start(platform)) {
		node->detach(platform);
		node->release();
		IOLog("bt: the registry entry \"bluetooth\" did not start\n");
		return;
	}
	node->registerService();
	IOLog("bt: the registry entry \"bluetooth\" has the controller's address\n");
}
