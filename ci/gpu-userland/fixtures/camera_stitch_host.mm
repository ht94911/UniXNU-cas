// SPDX-License-Identifier: MIT
// Production stitching serialization and the system compiler, without a GPU.
#import "../NativeBridgeMetalFrontend.mm"
#include <cassert>

@interface NBStitchTextDevice : NBDeviceMethods
@end
@implementation NBStitchTextDevice
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs {
 Class original=objc_getClass("_MTLDevice");
 Method method=class_getInstanceMethod(original,@selector(newDagStringWithGraphs:));
 assert(method);
 using Make=NSString *(*)(id,SEL,NSArray *);
 return ((Make)method_getImplementation(method))(self,@selector(newDagStringWithGraphs:),graphs);
}
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
 // Keep the real compiler output without registering or opening a GPU.
 const void *bytes=nullptr;size_t size=0;
 dispatch_data_t mapped=dispatch_data_create_map(data,&bytes,&size);
 NBLibrary *library=[NBLibrary new];library.storage=mapped;
 library.bytes=(const uint8_t *)bytes;library.length=size;
 if(error)*error=nil;
 return (id<MTLLibrary>)library;
}
@end

static NSData *functionLibrary(NSData *sumAir,NSData *productAir) {
 const bool shared=[sumAir isEqualToData:productAir];
 std::vector<uint8_t> entries;
 for(const char *name:{"sum2","product2"}) {
  std::vector<uint8_t> entry(4);
  auto tag=[&](const char *key,const void *value,uint16_t size) {
   entry.insert(entry.end(),key,key+4);
   entry.push_back((uint8_t)size);entry.push_back((uint8_t)(size>>8));
   const uint8_t *p=(const uint8_t *)value;entry.insert(entry.end(),p,p+size);
  };
  NSData *air=!strcmp(name,"sum2")?sumAir:productAir;
  const uint8_t type=5;uint64_t size=air.length,offsets[3]={0,0,shared || air==sumAir?0:(uint64_t)sumAir.length};
  tag("NAME",name,(uint16_t)(strlen(name)+1));tag("TYPE",&type,1);
  tag("MDSZ",&size,sizeof size);tag("OFFT",offsets,sizeof offsets);
  entry.insert(entry.end(),{'E','N','D','T'});
  const uint32_t entrySize=(uint32_t)entry.size();memcpy(entry.data(),&entrySize,4);
  entries.insert(entries.end(),entry.begin(),entry.end());
 }
 uint8_t header[88]={};memcpy(header,"MTLB",4);header[10]=1;
 const uint64_t list=88,listSize=entries.size(),bitcode=88+4+listSize,bytes=sumAir.length+(shared?0:productAir.length);
 memcpy(header+24,&list,8);memcpy(header+32,&listSize,8);
 memcpy(header+72,&bitcode,8);memcpy(header+80,&bytes,8);
 NSMutableData *data=[NSMutableData dataWithBytes:header length:88];
 const uint32_t count=2;[data appendBytes:&count length:4];
 [data appendBytes:entries.data() length:entries.size()];[data appendData:sumAir];
 if(!shared)[data appendData:productAir];return data;
}

static MTLFunctionStitchingInputNode *input(NSUInteger index) {
 return [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:index];
}
static MTLFunctionStitchingFunctionNode *node(NSString *name,NSArray *arguments) {
 return [[MTLFunctionStitchingFunctionNode alloc] initWithName:name arguments:arguments controlDependencies:@[]];
}

int main(int argc,char **argv) {@autoreleasepool {
 assert(argc==2 || argc==3);setvbuf(stdout,nullptr,_IONBF,0);
 NSData *air=[NSData dataWithContentsOfFile:@(argv[1])];assert(air.length);
 NSData *productAir=argc==3?[NSData dataWithContentsOfFile:@(argv[2])]:air;assert(productAir.length);
 const bool shared=[air isEqualToData:productAir];
 NSData *storage=functionLibrary(air,productAir);NBLibrary *library=[NBLibrary new];library.storage=storage;
 library.bytes=(const uint8_t *)storage.bytes;library.length=storage.length;
 NBFunction *sum=(NBFunction *)[library newExternFunctionWithName:@"sum2"];
 NBFunction *product=(NBFunction *)[library newExternFunctionWithName:@"product2"];
 NBFunction *again=(NBFunction *)[library newExternFunctionWithName:@"sum2"];
 assert(sum && product && again && sum!=again);
 assert([functionBitcode(sum) isEqualToData:functionBitcode(product)]==shared);
 assert((!memcmp(sum.bitCodeHash,product.bitCodeHash,32))==shared);
 assert(!memcmp(sum.bitCodeHash,again.bitCodeHash,32));
 uint8_t digest[32];CC_SHA256(air.bytes,(CC_LONG)air.length,digest);
 assert(!memcmp(sum.bitCodeHash,digest,32));
 CC_SHA256(productAir.bytes,(CC_LONG)productAir.length,digest);
 assert(!memcmp(product.bitCodeHash,digest,32));
 MTLFunctionStitchingFunctionNode *add=node(@"sum2",@[input(0),input(1)]);
 MTLFunctionStitchingFunctionNode *multiply=node(@"product2",@[add,input(2)]);
 MTLFunctionStitchingGraph *graph=[[MTLFunctionStitchingGraph alloc] initWithFunctionName:@"fused"
     nodes:@[add] outputNode:multiply attributes:@[]];
 NBStitchTextDevice *device=[NBStitchTextDevice new];
 MTLStitchedLibraryDescriptor *descriptor=[MTLStitchedLibraryDescriptor new];
 descriptor.functionGraphs=@[graph];descriptor.functions=@[(id<MTLFunction>)sum,(id<MTLFunction>)product,(id<MTLFunction>)again];
 NSError *error=nil;
 NBLibrary *made=(NBLibrary *)[(id<MTLDevice>)device newLibraryWithStitchedDescriptor:descriptor error:&error];
 if(!made) {fprintf(stderr,"Compiler rejected shared AIR: %s\n",error.localizedDescription.UTF8String);return 1;}
 assert(!error);
 NSData *stitched=[NSData dataWithBytes:made.bytes length:made.length];
 assert(stitched.length>88 && !memcmp(stitched.bytes,"MTLB",4));
 const char *output=getenv("CAMERA_STITCH_OUT");
 assert(output && [stitched writeToFile:@(output) atomically:NO]);
 uint32_t functions=0;uint64_t table=0;
 memcpy(&table,(const uint8_t *)stitched.bytes+24,8);assert(table<stitched.length-4);
 memcpy(&functions,(const uint8_t *)stitched.bytes+table,4);assert(functions>0);
 bool fused=false;
 assert(forEachLibraryEntry(made,[&](const std::string &name,int) {fused|=name=="fused";return true;}));
 assert(fused);
 printf("PASS Camera %s AIR stitching: production request; original DAG; repeated function object; compiler output %u functions\n",shared?"shared":"distinct",functions);
 return 0;
}}
