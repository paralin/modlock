package scriptc

// The instructions the added functions use.
const (
	// opUnreachable traps.
	opUnreachable = 0x00
	// opBlock opens a block that br leaves.
	opBlock = 0x02
	// opLoop opens a block that br repeats.
	opLoop = 0x03
	// opIf runs a block when the operand is nonzero.
	opIf = 0x04
	// opElse opens the block run when opIf's operand is zero.
	opElse = 0x05
	// opEnd closes a block or the function.
	opEnd = 0x0b
	// opBr jumps to a block's label.
	opBr = 0x0c
	// opBrIf jumps to a block's label when the operand is nonzero.
	opBrIf = 0x0d
	// opCall calls a function by index.
	opCall = 0x10
	// opDrop discards the operand.
	opDrop = 0x1a
	// opSelect chooses the first operand when the third is nonzero.
	opSelect = 0x1b
	// opLocalGet pushes a local.
	opLocalGet = 0x20
	// opLocalSet pops into a local.
	opLocalSet = 0x21
	// opLocalTee stores into a local and keeps the operand.
	opLocalTee = 0x22
	// opGlobalGet pushes a global.
	opGlobalGet = 0x23
	// opGlobalSet pops into a global.
	opGlobalSet = 0x24
	// opI32Load8U loads one byte.
	opI32Load8U = 0x2d
	// opI64Load32U loads four bytes into an i64.
	opI64Load32U = 0x35
	// opI32Store8 stores one byte.
	opI32Store8 = 0x3a
	// opMemorySize pushes the memory's size in pages.
	opMemorySize = 0x3f
	// opMemoryGrow adds pages and pushes the old size, or -1.
	opMemoryGrow = 0x40
	// opI32Const pushes an i32.
	opI32Const = 0x41
	// opI64Const pushes an i64.
	opI64Const = 0x42
	// opI32Eq compares two i32s for equality.
	opI32Eq = 0x46
	// opI32LtU compares two i32s, unsigned.
	opI32LtU = 0x49
	// opI32GtU compares two i32s, unsigned.
	opI32GtU = 0x4b
	// opI32GeU compares two i32s, unsigned.
	opI32GeU = 0x4f
	// opI32Add adds two i32s.
	opI32Add = 0x6a
	// opI32Sub subtracts two i32s.
	opI32Sub = 0x6b
	// opI32Or ors two i32s.
	opI32Or = 0x72
	// opI32Shl shifts an i32 left.
	opI32Shl = 0x74
	// opI32ShrU shifts an i32 right, unsigned.
	opI32ShrU = 0x76
	// opI64Or ors two i64s.
	opI64Or = 0x84
	// opI64Shl shifts an i64 left.
	opI64Shl = 0x86
	// blockVoid is the type of a block with no result.
	blockVoid = 0x40
)

// maxPanic is the longest panic message the panic sink logs, short enough
// that each length in its Log call fits a two-byte varint.
const maxPanic = 16000

// body is a function body under construction.
type body struct {
	// locals is the number of i32 locals after the parameters.
	locals uint32
	// code holds the instructions, without the final end.
	code []byte
}

// op appends instructions and immediates that are single bytes.
func (b *body) op(ops ...byte) {
	b.code = append(b.code, ops...)
}

// index appends an instruction that takes one index, such as a call.
func (b *body) index(op byte, index uint32) {
	b.code = appendU32(append(b.code, op), index)
}

// i32 appends an i32 constant.
func (b *body) i32(v int32) {
	b.code = appendS32(append(b.code, opI32Const), v)
}

// memory appends a load or a store with its alignment and offset.
func (b *body) memory(op byte, align, offset uint32) {
	b.code = appendU32(appendU32(append(b.code, op), align), offset)
}

// encode appends the finished body, with its size and locals, to out.
func (b *body) encode(out []byte) []byte {
	var encoded []byte
	if b.locals == 0 {
		encoded = append(encoded, 0)
	} else {
		encoded = append(appendU32(append(encoded, 1), b.locals), 0x7f)
	}
	encoded = append(append(encoded, b.code...), opEnd)
	return append(appendU32(out, uint32(len(encoded))), encoded...)
}

// initBody returns _initialize's replacement, which runs the module's
// initializer and the library's init, then allocates the event wrapper's
// two out-slots and keeps their address in the slots global.
func initBody(initialize, init, alloc, slots uint32) *body {
	// Initialize the module and the library, then allocate the out-slots.
	b := &body{}
	b.index(opCall, initialize)
	b.index(opCall, init)
	b.i32(8)
	b.index(opCall, alloc)
	b.index(opGlobalSet, slots)
	return b
}

// eventBody returns modlock_event, which calls the event export with the
// call's size and the out-slots, then packs the reply's address and length
// as address<<32 | length. The reply stays in scriptc's result arena until
// the next event, after the host has read it.
func eventBody(event, slots uint32) *body {
	// Call the event export with its out-slots.
	b := &body{}
	b.get(0)
	b.index(opGlobalGet, slots)
	b.index(opGlobalGet, slots)
	b.i32(4)
	b.op(opI32Add)
	b.index(opCall, event)

	// Pack the address from the first slot and the length from the second.
	b.index(opGlobalGet, slots)
	b.memory(opI64Load32U, 2, 0)
	b.op(opI64Const, 32, opI64Shl)
	b.index(opGlobalGet, slots)
	b.memory(opI64Load32U, 2, 4)
	b.op(opI64Or)
	return b
}

// The panic sink's parameters and locals.
const (
	// panicMessage is the address of scriptc's panic message.
	panicMessage = iota
	// panicLength is the message's length.
	panicLength
	// panicAt is where the Log call begins, written backwards from the
	// message's copy.
	panicAt
	// panicEnd is the end of the Log call.
	panicEnd
	// panicIndex counts the bytes copied.
	panicIndex
	// panicByte is the byte being copied.
	panicByte
	// panicVarint is the length being encoded.
	panicVarint
)

// panicBody returns the sink that replaces scriptc.panic. scriptc's message
// opens with 0x01 and separates the text, the code and the symbol with
// 0x1f. The sink logs the message through hostCall as a Log call and traps.
// The library is poisoned by then and cannot allocate, so the sink builds
// the call in a page it grows past the heap. When memory is at its limit,
// as it is when the panic reports that the heap ran out, the sink builds the
// call over the memory's last page instead, or over its first when the
// message ends in the last: the instance never runs again after the trap, so
// nothing reads what the call overwrites.
func panicBody(hostCall uint32) *body {
	// Test whether a nonempty message opens with 0x01.
	b := &body{locals: 5}
	b.get(panicLength)
	b.op(opIf, blockVoid)
	b.get(panicMessage)
	b.memory(opI32Load8U, 0, 0)
	b.i32(1)
	b.op(opI32Eq, opIf, blockVoid)

	// Drop the 0x01 when it does.
	b.add(panicMessage, 1)
	b.add(panicLength, -1)
	b.op(opEnd, opEnd)

	// Keep the message short enough for two-byte lengths.
	b.get(panicLength)
	b.i32(maxPanic)
	b.get(panicLength)
	b.i32(maxPanic)
	b.op(opI32LtU, opSelect)
	b.set(panicLength)

	// Grow a page for the call.
	b.i32(1)
	b.op(opMemoryGrow, 0)
	b.tee(panicAt)
	b.i32(-1)
	b.op(opI32Eq, opIf, blockVoid)

	// When memory cannot grow, take its last page.
	b.op(opMemorySize, 0)
	b.i32(1)
	b.op(opI32Sub)
	b.set(panicAt)

	// Take the first page when the message ends in the last.
	b.get(panicMessage, panicLength)
	b.op(opI32Add)
	b.get(panicAt)
	b.i32(16)
	b.op(opI32Shl, opI32GtU, opIf, blockVoid)
	b.i32(0)
	b.set(panicAt)
	b.op(opEnd, opEnd)

	// Place the message's copy 16 bytes into the page, past room for the
	// call's header.
	b.get(panicAt)
	b.i32(16)
	b.op(opI32Shl)
	b.i32(16)
	b.op(opI32Add)
	b.set(panicAt)

	// Find the end of the copy, which ends the call.
	b.get(panicAt, panicLength)
	b.op(opI32Add)
	b.set(panicEnd)

	// Loop over the message until every byte is copied.
	b.op(opBlock, blockVoid, opLoop, blockVoid)
	b.get(panicIndex, panicLength)
	b.op(opI32GeU)
	b.index(opBrIf, 1)

	// Address the copy's byte and load the message's.
	b.get(panicAt, panicIndex)
	b.op(opI32Add)
	b.i32(' ')
	b.get(panicMessage, panicIndex)
	b.op(opI32Add)
	b.memory(opI32Load8U, 0, 0)
	b.tee(panicByte)

	// Store a space in place of a 0x1f separator, or else the byte.
	b.get(panicByte)
	b.i32(0x1f)
	b.op(opI32Eq, opSelect)
	b.memory(opI32Store8, 0, 0)

	// Step to the next byte.
	b.add(panicIndex, 1)
	b.index(opBr, 0)
	b.op(opEnd, opEnd)

	// Write LogRequest.message's tag and length before the copy.
	b.get(panicLength)
	b.set(panicVarint)
	b.varint()
	b.prepend(0x0a)

	// Write Call.request's tag and the LogRequest's length before it.
	b.get(panicEnd, panicAt)
	b.op(opI32Sub)
	b.set(panicVarint)
	b.varint()
	b.prepend(0x12)

	// Write Call.method, "Log", first.
	for _, c := range []byte{'g', 'o', 'L', 3, 0x0a} {
		b.prepend(c)
	}

	// Hand the call to the host, then trap.
	b.get(panicAt, panicEnd, panicAt)
	b.op(opI32Sub)
	b.index(opCall, hostCall)
	b.op(opDrop, opUnreachable)
	return b
}

// get pushes each local in order.
func (b *body) get(locals ...uint32) {
	for _, local := range locals {
		b.index(opLocalGet, local)
	}
}

// set pops into a local.
func (b *body) set(local uint32) {
	b.index(opLocalSet, local)
}

// tee stores into a local and keeps the operand.
func (b *body) tee(local uint32) {
	b.index(opLocalTee, local)
}

// add adds v to the i32 local.
func (b *body) add(local uint32, v int32) {
	b.get(local)
	b.i32(v)
	b.op(opI32Add)
	b.set(local)
}

// prepend moves the panic sink's cursor back one byte and stores c there.
func (b *body) prepend(c byte) {
	b.add(panicAt, -1)
	b.get(panicAt)
	b.i32(int32(c))
	b.memory(opI32Store8, 0, 0)
}

// varint writes the panic sink's varint local before its cursor, in one byte
// below 128 and in two otherwise.
func (b *body) varint() {
	// Write a small length in one byte.
	b.get(panicVarint)
	b.i32(128)
	b.op(opI32LtU, opIf, blockVoid)
	b.add(panicAt, -1)
	b.get(panicAt, panicVarint)
	b.memory(opI32Store8, 0, 0)

	// Write a larger one's low seven bits with the continuation bit.
	b.op(opElse)
	b.add(panicAt, -2)
	b.get(panicAt, panicVarint)
	b.i32(0x80)
	b.op(opI32Or)
	b.memory(opI32Store8, 0, 0)

	// Write the rest in the second byte.
	b.get(panicAt, panicVarint)
	b.i32(7)
	b.op(opI32ShrU)
	b.memory(opI32Store8, 0, 1)
	b.op(opEnd)
}
