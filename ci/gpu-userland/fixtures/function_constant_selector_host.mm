// SPDX-License-Identifier: MIT
// Exercise the actual Objective-C constant object returned by the frontend.
#import "../NativeBridgeMetalFrontend.mm"
#include <cassert>

@protocol NBRequiredFunctionConstant
- (BOOL)required;
@end

int main() {
    @autoreleasepool {
        NBFunctionConstant *constant=[NBFunctionConstant new];
        SEL selector=sel_registerName("required");
        if (![constant respondsToSelector:selector]) {
            puts("FAIL missing production MTLFunctionConstant required selector");
            return 1;
        }
        unsigned cases=0;
        for (BOOL required : {NO,YES,NO}) {
            constant.required=required;
            assert([(id<NBRequiredFunctionConstant>)constant required]==required);++cases;
            assert(constant.isRequired==required);++cases;
            assert(((BOOL (*)(id,SEL))objc_msgSend)(constant,selector)==required);++cases;
        }
        constant.name=@"real_constant";constant.index=7;constant.type=MTLDataTypeInt;
        assert([constant.name isEqual:@"real_constant"] && constant.index==7 && constant.type==MTLDataTypeInt);++cases;
        printf("PASS function constant selector: %u production getter and native message cases\n",cases);
    }
    return 0;
}
