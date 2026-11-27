/* SPDX-License-Identifier: MIT */
#ifndef A840_MTL_PIPELINE_PROVIDER_H
#define A840_MTL_PIPELINE_PROVIDER_H

#import "A840MTLDevice.h"

@interface A840MTLLibrary : NSObject <MTLLibrary>
- (instancetype)initWithDevice:(A840MTLDevice *)device data:(NSData *)data error:(NSError **)error;
@property(nonatomic, copy, readonly) NSData *metallibData;
- (void *)parsedNmbLibrary;
@end

@interface A840MTLFunction : NSObject <MTLFunction>
- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType;
- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
                 constantValues:(MTLFunctionConstantValues *)constantValues
                          error:(NSError **)error;
- (instancetype)initWithLibrary:(A840MTLLibrary *)library
                     sourceName:(NSString *)sourceName
                specializedName:(NSString *)specializedName
                   functionType:(MTLFunctionType)functionType
                 constantValues:(MTLFunctionConstantValues *)constantValues
                    specialized:(BOOL)specialized
                          error:(NSError **)error;
@property(nonatomic, strong, readonly) A840MTLLibrary *ownerLibrary;
@property(nonatomic, copy, readonly) NSString *sourceName;
@property(nonatomic, copy, readonly) NSString *specializedName;
@property(nonatomic, readonly) BOOL isSpecialized;
- (NSData *)extractBitcodeData;
- (const void *)bitCodeHash;
@end

@interface A840MTLNativePipelineProvider : NSObject <A840MTLPipelineProvider>
- (instancetype)init;
@end

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("default")))
id<MTLDevice> a840_metal_create_direct_device(NSError **error);

__attribute__((visibility("default")))
id<MTLDevice> a840_metal_create_mock_device(A840BackendTransport backend, NSError **error);

#ifdef __cplusplus
}
#endif

#endif /* A840_MTL_PIPELINE_PROVIDER_H */
