/*
 * cb_test: Bluetooth LE through CoreBluetooth, the way an application uses it: wait for Bluetooth to be on, scan,
 * say what is found, and connect to the first device whose name has the word given, to list its services.
 *
 *   cb_test [SECONDS [WORD [SERVICE...]]]      (40, no connection, every device)
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

@interface Scanner : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (strong) CBCentralManager *central;
@property (strong) NSMutableSet *seen;
@property (strong) CBPeripheral *chosen;
@property (copy) NSString *word;
@property (strong) NSArray *services;
@property unsigned found, named;
@end

@implementation Scanner
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	printf("cb_test: Bluetooth state %ld (5 is on, 4 off, 3 not allowed, 2 not there)\n", (long)central.state);
	if (central.state == CBManagerStatePoweredOn) {
		[central scanForPeripheralsWithServices:self.services options:nil];
		printf("cb_test: scanning%s\n", self.services != nil ? " for the services given" : "");
	}
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)data RSSI:(NSNumber *)rssi
{
	NSString *name = peripheral.name ?: data[CBAdvertisementDataLocalNameKey];

	if ([self.seen containsObject:peripheral.identifier]) {
		return;
	}
	[self.seen addObject:peripheral.identifier];
	self.found++;
	if (name != nil) {
		self.named++;
		printf("cb_test: found \"%s\", %d dBm\n", name.UTF8String, rssi.intValue);
	}
	if (self.chosen == nil && self.word != nil && name != nil &&
	    [name rangeOfString:self.word options:NSCaseInsensitiveSearch].location != NSNotFound) {
		self.chosen = peripheral;
		peripheral.delegate = self;
		printf("cb_test: connecting to \"%s\"\n", name.UTF8String);
		[central connectPeripheral:peripheral options:nil];
	}
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
	printf("cb_test: CONNECTED; asking for its services\n");
	[peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
	printf("cb_test: the connection failed: %s\n", error.localizedDescription.UTF8String);
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
	printf("cb_test: disconnected%s%s\n", error != nil ? ": " : "", error != nil ? error.localizedDescription.UTF8String : "");
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
	if (error != nil) {
		printf("cb_test: its services: %s\n", error.localizedDescription.UTF8String);
		return;
	}
	printf("cb_test: %lu services:", (unsigned long)peripheral.services.count);
	for (CBService *service in peripheral.services) {
		printf(" %s", service.UUID.UUIDString.UTF8String);
	}
	printf("\n");
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		int seconds = argc > 1 ? atoi(argv[1]) : 40;
		Scanner *scanner = [Scanner new];

		setvbuf(stdout, NULL, _IOLBF, 0);
		scanner.seen = [NSMutableSet set];
		scanner.word = argc > 2 ? @(argv[2]) : nil;
		if (argc > 3) {
			NSMutableArray *services = [NSMutableArray array];

			for (int i = 3; i < argc; i++) {
				[services addObject:[CBUUID UUIDWithString:@(argv[i])]];
			}
			scanner.services = services;
		}
		scanner.central = [[CBCentralManager alloc] initWithDelegate:scanner queue:dispatch_get_main_queue()];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			printf("cb_test: %u devices found in %d s, %u with a name; Bluetooth state %ld\n", scanner.found, seconds, scanner.named,
			    (long)scanner.central.state);
			exit(0);
		});
		[[NSRunLoop mainRunLoop] run];
	}
	return 0;
}
