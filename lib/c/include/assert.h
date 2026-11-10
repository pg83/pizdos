#include <__base.h>

#undef assert

#ifdef NDEBUG
#define assert(x) (void)0
#else
#define assert(x) ((void)((x) || (__assert_fail(#x, __FILE__, __LINE__, __func__), 0)))
#endif

#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

__BEGIN_DECLS

__NORETURN void __assert_fail(const char*, const char*, int, const char*);

__END_DECLS
