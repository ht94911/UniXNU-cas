# GPU052 resource directory continuation

GPU050 retains the CoreImage null-program failure while its kernel still
reports an upper union directory's EOF to getdirentries64 callers. The original
iPhone CFBundle directory query uses readdir, which remembers this EOF and can
stop before the lower volume supplies the bundle resources.

Reuse the unchanged upstream native-union-readdir-eof.patch from framework
f105b318080f6990c16cd5ab18176b1c66c51912, already exercised by camera060/062.
The XNU base and preceding VFS patches are identical to GPU050. The same
production function passed 20 host cases for one, two and three volumes,
EOF with returned entries, an empty upper directory, and lookup/MAC/open/ref
failures with balanced ownership. Camera062's actual DAS process stopped
repeating its missing-model exception; this is evidence of the resource path,
not proof that CoreImage has the same failure.

Only this directory correction is added. GPU051's allocator trial remains
separate. Before a GPU052 boot, compile the current native target, preserve the
accepted WiFi/Bluetooth/brightness image, and check the existing CoreImage
library evidence and icon-service behavior. Do not claim that finding the
libraries proves shader creation or rendering succeeds.
