## Phase 1 explanation
### File offset and VM Address
- File offset: How many bytes from beginning of the bytes
- VM Address: What memory address this will have when the app is running => **position in memory at run time**
Why does it matter:
The critical thing is that **all pointers stored inside objC metadata point to VM address, not the file offset**. For ex:
Inside class_ro_t the name field is a pointer like 0x10000560c. That means:
    "The class name string is at virtual address 0x10000560c"
But you cannot use 0x10000560c to index into your mmap buffer directly — that number is enormous and would crash immediately. You need to convert it to a file offset first:
fileOffset = vmAddr - segmentVmAddr + segmentFileOff
           = 0x10000560c - 0x100000000 + 0x0
           = 0x560c

```
```

```
BINARY FILE (mmap buffer)          VIRTUAL MEMORY (at runtime)
─────────────────────────          ──────────────────────────
byte[0x0000]  ──────────────────►  0x100000000  (__TEXT start)
byte[0x560c]  ──────────────────►  0x10000560c  (__objc_classname)
byte[0x5690]  ──────────────────►  0x100005690  (__objc_methname)
byte[0x8000]  ──────────────────►  0x100008000  (__DATA_CONST start)
byte[0x8158]  ──────────────────►  0x100008158  (__objc_classlist)
byte[0xc000]  ──────────────────►  0x10000c000  (__DATA start)

Conversion formula:
  fileOffset = vmAddr - segment.vmAddr + segment.fileOff
  vmAddr     = fileOffset - segment.fileOff + segment.vmAddr
```
```
```
```
```
```
```
```
```


### __PAGEZERO
SEGMENT  __PAGEZERO   fileOff=0x0   vmAddr=0x0   size=0x0

__PAGEZERO is special — it occupies virtual address 0x0 to 0x100000000 (4GB) at runtime to catch null pointer dereferences, but it has zero bytes on disk (fileOff=0x0, size=0x0). This is why arm64 apps always start their real code at 0x100000000 — everything below that is the null-guard page.


## Phase 2 explanation

```
```
```
objc_structs.h          objc_types.h
──────────────          ────────────
Raw binary layout       Rich C++ objects

class_t_64              ObjcClass
  └─ bits.dataPtr()  →    name (StringInData)
                           methods[]
class_ro_64                 └─ ObjcMethod
  └─ name (uint64)              name (StringInData)
  └─ baseMethodList             methType (StringInData)
  └─ ivars               ivars[]
  └─ baseProperties      properties[]

method_t<uint64_t>      ObjcMethod
  └─ name (uint64)  →     name.value   = "viewDidLoad"
  └─ types (uint64)       name.fileOffset = 0x5690
                          methType.value  = "v16@0:8"

category_t<uint64_t>    ObjcCategory
  └─ name (uint64)  →     name.value   = "MyCategory"

protocol_t<uint64_t>    ObjcProtocol
  └─ name (uint64)  →     name.value   = "MyProtocol"
```

1. Why two separate structs (class_ro_64 / class_ro_32) instead of a template?

Because the reserved field only exists in the 64-bit version. A template class_ro_t<uint64_t> would have the same layout as class_ro_t<uint32_t> with different pointer sizes — there is no clean way to conditionally include a field in a template without specialization. The original Swift code solved this the same way — two distinct structs.

2. Why does StringInData store fileOffset?

This is the critical insight from MachStrings.swift. When Phase 6 needs to patch a class name, it must know exactly where in the buffer to write the new bytes. If we only stored the string value, we would have to scan the buffer again to find it — which is slow and could match the wrong occurrence. By storing fileOffset at extraction time, patching becomes a direct memcpy to slice.data + fileOffset.

3. Why the relative_method_t struct?

Your test binary has __objc_methlist which is the Xcode 14+ relative method list format. The original Swift project did not handle this (it predates it). We define the struct now so Phase 3 can use it when it detects the relative method list flag (bits & 0x80000000).

4. #pragma pack(push, 1) — why?

Compilers insert padding between struct fields to align them. For example, a uint32_t followed by a uint64_t might get 4 bytes of padding inserted between them. But Apple's binary format has no such padding — the fields are packed exactly as defined. #pragma pack(1) tells the compiler to match the binary layout byte-for-byte.

## Phase 3 explanation

The exact names depend on what is in your test binary. The critical things to verify are:

Class count matches otool -oV testckey_objc | grep "^[[:space:]]*name" output
fileOffset values for class names all fall within [0x560c, 0x560c+0x84) (the __objc_classname range from Phase 1)
Selector count is reasonable (should be close to strings testckey_objc | wc -l)

