/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_LIBRARY_AND_ARCHIVE_H
#define A840_MTL_LIBRARY_AND_ARCHIVE_H

#import "A840MTLDevice.h"
#include "../core/a840_system_frontend_core.h"

@interface A840MTLBinaryArchive : NSObject <MTLBinaryArchive>
- (instancetype)initWithOwner:(A840MTLDevice *)owner
                   descriptor:(MTLBinaryArchiveDescriptor *)descriptor
                        error:(NSError **)error;
@property(nonatomic, strong, readonly) A840MTLDevice *owner;
@property(nonatomic, readonly) A840SysArchiveHandle archiveHandle;
@end

@interface A840MTLStitchedLibraryLoader : NSObject
+ (id<MTLLibrary>)libraryWithDispatchData:(dispatch_data_t)data
                                   device:(A840MTLDevice *)device
                                    error:(NSError **)error;
+ (id<MTLLibrary>)libraryWithSource:(NSString *)source
                            options:(MTLCompileOptions *)options
                             device:(A840MTLDevice *)device
                              error:(NSError **)error;
+ (id<MTLLibrary>)libraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                         device:(A840MTLDevice *)device
                                          error:(NSError **)error;
@end

#endif /* A840_MTL_LIBRARY_AND_ARCHIVE_H */
