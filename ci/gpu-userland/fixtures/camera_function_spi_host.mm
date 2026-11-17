// SPDX-License-Identifier: MIT
// Actual production cache selectors and function argument reflection.
#import "../NativeBridgeMetalFrontend.mm"
#include <cassert>

@interface NBCameraCacheForwarding : NBLibrary
@property(nonatomic) unsigned calls;
@property(nonatomic,strong) NSString *askedName;
@property(nonatomic,strong) MTLFunctionConstantValues *askedValues;
@property(nonatomic,strong) NBFunction *answer;
@end
@implementation NBCameraCacheForwarding
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
 self.calls++;self.askedName=name;self.askedValues=values;
 if([name isEqualToString:@"available"]) {if(error)*error=nil;return (id<MTLFunction>)self.answer;}
 if(error)*error=[NSError errorWithDomain:@"authored-cache-fixture" code:7 userInfo:nil];
 return nil;
}
@end

static NSData *reflectionMetallib(NSData *bitcode,bool tagged) {
 std::vector<uint8_t> entry(4);
 auto tag=[&](const char *name,const void *bytes,uint16_t count) {
  entry.insert(entry.end(),name,name+4);entry.push_back(count&255);entry.push_back(count>>8);
  const uint8_t *at=(const uint8_t *)bytes;entry.insert(entry.end(),at,at+count);
 };
 const char *name="reflection_tables";uint8_t type=2;
 std::vector<uint8_t> wrapped(20+bitcode.length);
 const uint32_t fields[5]={0x0b17c0de,0,20,(uint32_t)bitcode.length,0};
 memcpy(wrapped.data(),fields,20);memcpy(wrapped.data()+20,bitcode.bytes,bitcode.length);
 uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(wrapped.data(),(CC_LONG)wrapped.size(),digest);
 const uint64_t size=wrapped.size(),offsets[3]={0,0,0},reflection=0;
 const uint16_t versions[4]={2,8,4,0};
 tag("NAME",name,strlen(name)+1);tag("TYPE",&type,1);tag("HASH",digest,sizeof digest);
 tag("MDSZ",&size,8);tag("OFFT",offsets,24);tag("VERS",versions,sizeof versions);
 if(tagged)tag("RFLT",&reflection,8);
 entry.insert(entry.end(),{'E','N','D','T'});
 uint32_t length=entry.size(),count=1;memcpy(entry.data(),&length,4);
 NSMutableData *data=[NSMutableData dataWithLength:88];uint8_t *head=(uint8_t *)data.mutableBytes;
 memcpy(head,"MTLB",4);head[4]=1;head[5]=0x80;head[6]=2;head[8]=8;head[11]=0x81;head[12]=26;
 const uint64_t list=88,functions=entry.size(),publicAt=list+4+functions+4,privateAt=publicAt+8,module=privateAt+8,total=module+wrapped.size(),metadataSize=8;
 memcpy(head+16,&total,8);memcpy(head+24,&list,8);memcpy(head+32,&functions,8);
 memcpy(head+40,&publicAt,8);memcpy(head+48,&metadataSize,8);memcpy(head+56,&privateAt,8);memcpy(head+64,&metadataSize,8);
 memcpy(head+72,&module,8);memcpy(head+80,&size,8);
 [data appendBytes:&count length:4];[data appendBytes:entry.data() length:entry.size()];
 [data appendBytes:"ENDT" length:4];const uint32_t mdSize=8;
 for(unsigned i=0;i<2;i++){[data appendBytes:&mdSize length:4];[data appendBytes:"ENDT" length:4];}
 [data appendBytes:wrapped.data() length:wrapped.size()];return data;
}

static NSData *publicArgumentsMetallib(NSData *payload) {
 std::vector<uint8_t> entry(4),metadata(4);
 auto tag=[](std::vector<uint8_t> &out,const char *name,const void *bytes,uint16_t count) {
  out.insert(out.end(),name,name+4);out.push_back(count&255);out.push_back(count>>8);
  const uint8_t *p=(const uint8_t *)bytes;out.insert(out.end(),p,p+count);
 };
 const char name[]="public_values";const uint8_t type=5;const uint64_t offsets[3]={0,0,0};
 tag(entry,"NAME",name,sizeof name);tag(entry,"TYPE",&type,1);tag(entry,"OFFT",offsets,24);entry.insert(entry.end(),{'E','N','D','T'});
 const uint8_t returned=6;tag(metadata,"RETR",&returned,1);
 tag(metadata,"ARGR",payload.bytes,(uint16_t)payload.length);metadata.insert(metadata.end(),{'E','N','D','T'});
 uint32_t entrySize=entry.size(),metadataSize=metadata.size(),count=1;
 memcpy(entry.data(),&entrySize,4);memcpy(metadata.data(),&metadataSize,4);
 NSMutableData *out=[NSMutableData dataWithLength:88];uint8_t *head=(uint8_t *)out.mutableBytes;
 memcpy(head,"MTLB",4);head[10]=1;
 uint64_t list=88,listSize=entry.size(),publicAt=list+4+listSize,publicSize=metadata.size();
 memcpy(head+24,&list,8);memcpy(head+32,&listSize,8);memcpy(head+40,&publicAt,8);memcpy(head+48,&publicSize,8);
 [out appendBytes:&count length:4];[out appendBytes:entry.data() length:entry.size()];[out appendBytes:metadata.data() length:metadata.size()];return out;
}

static NBFunction *publicFunction(NSData *payload) {
 NSData *bytes=publicArgumentsMetallib(payload);NBLibrary *library=[NBLibrary new];
 library.storage=bytes;library.bytes=(const uint8_t *)bytes.bytes;library.length=bytes.length;
 return (NBFunction *)[library newExternFunctionWithName:@"public_values"];
}

static unsigned publicArgumentCases() {
 fprintf(stderr,"camera-function-consumer: public metadata start\n");
 unsigned checks=0;
 // Authored scalar, vector, texture-array, sampler, array, pointer and struct.
 NSMutableData *payload=[NSMutableData data];uint8_t count=8;[payload appendBytes:&count length:1];
 auto argument=[&](const char *name,uint8_t type) {[payload appendBytes:name length:strlen(name)+1];[payload appendBytes:&type length:1];};
 argument("scalar",3);argument("vector",6);argument("texture",58);
 uint8_t element=3,flags=2;uint32_t textures=2;
 [payload appendBytes:&element length:1];[payload appendBytes:&textures length:4];[payload appendBytes:&flags length:1];
 argument("sampler",59);argument("array",2);uint32_t length=3,stride=8;element=4;
 [payload appendBytes:&length length:4];[payload appendBytes:&stride length:4];[payload appendBytes:&element length:1];
 argument("pointer",60);uint8_t access=0,alignment=16;uint64_t size=64;element=3;
 [payload appendBytes:&access length:1];[payload appendBytes:&alignment length:1];[payload appendBytes:&size length:8];[payload appendBytes:&element length:1];
 argument("structure",1);uint8_t members=1;[payload appendBytes:&members length:1];
 [payload appendBytes:"field" length:6];element=29;uint32_t index=7;
 [payload appendBytes:&element length:1];[payload appendBytes:&index length:4];argument("half",16);
 NBFunction *function=publicFunction(payload);NSArray *arguments=function.arguments;
 fprintf(stderr,"camera-function-consumer: public values count=%lu\n",(unsigned long)arguments.count);
 assert(function && !function.handle && arguments.count==8);++checks;
 NBArgument *scalar=arguments[0],*vector=arguments[1],*texture=arguments[2],*sampler=arguments[3],*array=arguments[4],*pointer=arguments[5],*structure=arguments[6];
 assert([scalar.name isEqualToString:@"scalar"] && (NSUInteger)scalar.type==18 && scalar.index==0 && scalar.isActive && !scalar.isArgument);++checks;
 assert(scalar.dataTypeDescription.dataType==MTLDataTypeFloat && vector.dataTypeDescription.dataType==MTLDataTypeFloat4);++checks;
 fprintf(stderr,"camera-function-consumer: scalar/vector types passed\n");
 MTLArrayType *textureArray=(MTLArrayType *)texture.dataTypeDescription;
 assert(textureArray.arrayLength==2 && textureArray.elementType==MTLDataTypeTexture && textureArray.stride==0);++checks;
 MTLTextureReferenceType *textureType=textureArray.elementTextureReferenceType;
 assert(textureType.textureDataType==MTLDataTypeFloat && textureType.textureType==MTLTextureType2D && textureType.access==MTLBindingAccessReadOnly);++checks;
 assert(sampler.dataTypeDescription.dataType==MTLDataTypeSampler);++checks;
 MTLArrayType *arrayType=(MTLArrayType *)array.dataTypeDescription;
 assert(arrayType.arrayLength==3 && arrayType.elementType==MTLDataTypeFloat2 && arrayType.stride==8);++checks;
 MTLPointerType *pointerType=(MTLPointerType *)pointer.dataTypeDescription;
 assert(pointerType.elementType==MTLDataTypeFloat && pointerType.alignment==16 && pointerType.dataSize==64);++checks;
 MTLStructType *structType=(MTLStructType *)structure.dataTypeDescription;MTLStructMember *member=[structType memberByName:@"field"];
 assert(structType.members.count==1 && member.dataType==MTLDataTypeInt && member.offset==0 && member.argumentIndex==7);++checks;
 assert(function.arguments==arguments && scalar.dataTypeDescription==scalar.dataTypeDescription);++checks;
 assert(function.returnType.dataType==MTLDataTypeFloat4 && function.returnType==function.returnType);++checks;
 uint8_t empty=0;assert(publicFunction([NSData dataWithBytes:&empty length:1]).arguments.count==0);++checks;
 NSMutableData *shortened=[payload mutableCopy];shortened.length--;assert(!publicFunction(shortened).arguments);++checks;
 const uint8_t missingNul[]={1,'x'};assert(!publicFunction([NSData dataWithBytes:missingNul length:sizeof missingNul]).arguments);++checks;
 return checks;
}

int main(int argc,char **argv) {@autoreleasepool {
 setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
 fprintf(stderr,"camera-function-consumer: entered argc=%d\n",argc);
 assert(argc==4 || argc==2);unsigned checks=publicArgumentCases();
 if(argc==2) {
  assert(!strcmp(argv[1],"public"));printf("PASS Camera public metadata: %u cases\n",checks);return 0;
 }
 NBCameraCacheForwarding *cache=[NBCameraCacheForwarding new];cache.answer=[NBFunction new];
 MTLFunctionConstantValues *values=[MTLFunctionConstantValues new];NSError *error=nil;id opaque=[NSObject new];
 assert([cache newFunctionWithName:@"available" constantValues:values pipelineLibrary:opaque error:&error]==(id<MTLFunction>)cache.answer && !error);++checks;
 assert(cache.calls==1 && cache.askedValues==values && [cache.askedName isEqualToString:@"available"]);++checks;
 assert(![cache newFunctionWithName:@"absent" constantValues:values functionCache:opaque error:&error] && error.code==7);++checks;
 assert(cache.calls==2 && cache.askedValues==values);++checks;
 NBFunction *unbacked=[NBFunction new];assert(!unbacked.arguments && unbacked.argumentsRead);++checks;
 fprintf(stderr,"camera-function-consumer: caches and no reflection passed; opening host runtime\n");
 NmbDevice *device=nmb_device_open(argv[1],argv[2]);assert(device);++checks;
 NSData *air=[NSData dataWithContentsOfFile:@(argv[3])];assert(air.length);++checks;
 for(bool tagged:{false,true}) {
  NSData *bytes=reflectionMetallib(air,tagged);
  NmbLibrary *handle=nmb_library_new(device,bytes.bytes,bytes.length,false);assert(handle);++checks;
  fprintf(stderr,"camera-function-consumer: actual reflection library tagged=%d loaded\n",tagged);
  NBLibrary *library=[NBLibrary new];library.handle=handle;library.storage=bytes;library.bytes=(const uint8_t *)bytes.bytes;library.length=bytes.length;
  NBFunction *function=(NBFunction *)[library newFunctionWithName:@"reflection_tables"];assert(function);++checks;
  NSArray *arguments=function.arguments;
  if(tagged) {
   assert(arguments.count==2);++checks;
   NBArgument *first=arguments[0],*second=arguments[1];
   assert([first.name isEqualToString:@"table"] && first.index==0);++checks;
   assert([second.name isEqualToString:@"out"] && second.index==1 && second.bufferDataSize==4);++checks;
   assert(function.arguments==arguments && function.argumentsRead);++checks;
  } else {assert(!arguments && function.argumentsRead);++checks;}
  NmbReflection *missing=(NmbReflection *)(uintptr_t)1;
  assert(!nmb_library_function_reflection(handle,"absent",&missing) && !missing);++checks;
 }
 printf("PASS Camera function SPI: %u production cache-forwarding and actual reflection cases\n",checks);
 return 0;
}}
