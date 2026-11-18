// SPDX-License-Identifier: MIT
// Exercise the production bridge's option decoding without opening a GPU.
use super::{heap_options, options};

#[test]
fn original_heap_hints_preserve_resource_fields() {
    let accepted = [
        0x200, 0x200200, 0x400200, 0x600200, 0x1000200, 0x1600200,
        0x600021, 0x600111, 0x600221,
    ];
    for value in accepted {
        let actual = heap_options(value).expect("supported heap options").raw();
        assert_eq!(actual, value & !0x600000);
    }
    for value in [0x400, 0x100200, 0x800200, 0x600203, 0x600240, u64::MAX] {
        assert!(heap_options(value).is_err(), "invalid fields and unknown bits remain refused");
    }
    assert!(options(0x600200).is_err(), "other resource constructors keep their input rules");
    println!("PASS heap options: 16 production cases; iPhone granularity hints removed; storage/cache/hazard/read-only and ordinary resource decoding preserved");
}
