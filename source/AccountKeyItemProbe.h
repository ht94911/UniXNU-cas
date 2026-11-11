#import <Foundation/Foundation.h>

// One serial caller; queries only the two public test items in the own group.
typedef void (^AA27ProbeProgress)(NSDictionary *snapshot);
NSDictionary *AA27PerformProbe(NSString *operation, AA27ProbeProgress progress);
