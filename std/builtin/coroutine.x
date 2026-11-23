// the coroutine part that .x can use. a go in .x starts a .n fn, and its future lives outside the gc, in one
// malloc block together with the result, which the .x code frees with deinit after await. await is here
// rather than in coroutine.n, so .n and .x await a future the same way.
import libc

#linkid rt_coroutine_async
fn coroutine_async(anyptr function, int flag, anyptr future):anyptr

#linkid rt_coroutine_await
fn coroutine_await(anyptr future):void!

#linkid rt_in_heap
fn in_heap(anyptr addr):bool

// the compiler lowers a go in .x to x_async, function is the closure the coroutine runs
pub fn x_async<T>(anyptr function, int flag):ptr<future_t<T>> {
    int header = @sizeof(future_t<T>)
    var fu = libc.calloc(1, (header + @sizeof(T)) as u64) as ptr<future_t<T>>
    fu.size = @sizeof(T)
    fu.result = (fu as anyptr + header as anyptr) as ptr<T>
    fu.co = coroutine_async(function, flag, fu as anyptr)
    return fu
}

// a go statement in .x keeps no future, .x code could never free one it does not hold
pub fn x_go(anyptr function, int flag) {
    coroutine_async(function, flag, null)
}

// fails with the errset the coroutine failed with. it yields the coroutine of the caller, or blocks the
// thread when there is none (main in .x)
#where T:nonvoid
pub fn future_t<T>.await(*self):T! {
    coroutine_await(self as anyptr)
    return *self.result
}

pub fn future_t<T>.await_void(*self):void! {
    coroutine_await(self as anyptr)
}

// frees a future that a go in .x returned, once it was awaited. a future from a go in .n belongs to the
// gc, so deinit leaves it alone
pub fn future_t<T>.deinit(*self) {
    if in_heap(self as anyptr) {
        return
    }
    libc.free(self as anyptr)
}
