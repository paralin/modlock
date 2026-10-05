;; host.wat stands in for the host while Wizer initializes the Python
;; runtime, which calls neither import until a mod starts.
(module
  (func (export "host_call") (param i32 i32) (result i32) unreachable)
  (func (export "host_read") (param i32 i32) unreachable))
