/* SPDX-License-Identifier: MIT */
#ifndef A840_IOKIT_TRANSPORT_H
#define A840_IOKIT_TRANSPORT_H

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include "a840_iokit_wire.h"

/* Runtime-loaded IOKit transport supporting direct selector, sandbox selector
 * tunneling, mailbox fallback, and host mock backends for Mac CI validation. */
@interface A840IokitTransport : NSObject
- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithConnection:(mach_port_t)connection
                    takesOwnership:(BOOL)ownsConnection
                             error:(NSError **)error NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithDefaultAcceleratorError:(NSError **)error;
- (instancetype)initWithBackend:(A840BackendTransport)backend;
- (BOOL)reportMetadataLine:(NSString *)line;
@property(nonatomic, readonly) A840IokitWire *wire;
@property(nonatomic, readonly) A840BackendTransport backend;
@end

#endif /* A840_IOKIT_TRANSPORT_H */
