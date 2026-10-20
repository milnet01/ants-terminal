// ANTS-5734 ship 3 (objc): a definition and a call of shipThreeBatchTwo.
@implementation Probe
- (void)shipThreeBatchTwo {}
- (void)objcCaller { [self shipThreeBatchTwo]; }
@end
