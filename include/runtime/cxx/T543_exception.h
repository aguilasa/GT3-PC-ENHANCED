/* T543_exception.h - exception, bad_exception and the C++ exception runtime of egcs
 * (cp/exception.cc; translation unit T543, unit u134).
 *
 * Same machine code as the bootstrap's runtime/exception.cc.  egcs ignores
 * `namespace std` (-fno-honor-std): the classes are mangled "9exception" /
 * "13bad_exception" and live at global scope.  Also in this unit, not
 * declared here (reached by compiler-emitted code or this unit only, or no
 * callers): std::terminate 0x00279CA8 (the landing pads of nothrow regions),
 * set_terminate 0x00279CD0, set_unexpected 0x00279CE0, unexpected 0x00279CF0,
 * __cp_exception_info 0x00279D08, __cp_eh_info 0x00279D30, __eh_free 0x00279DB0,
 * __cplus_type_matcher 0x00279DD0 (cp_eh_info.eh_info.match_function),
 * __uncatch_exception 0x00279F98, uncaught_exception 0x0027A280.
 */
#ifndef GAME_T543_EXCEPTION_H
#define GAME_T543_EXCEPTION_H

#include "game/game.h"
#include "runtime/libgcc/T542_eh.h"                       /* __eh_info (held by value) */
#include <stddef.h>

class type_info;                                /* T548_tinfo.h; references only */

/* Cleanup of a thrown object: an egcs destructor (object, in-charge flags);
 * flags 2 = destroy, do not delete. */
typedef void (*cleanup_fn)(void *, int);

/* The C++ information about an exception in flight, 0x30 bytes
 * (__eh_alloc(0x30) in __cp_push_exception); the list head is
 * eh_context.info, reached through __get_eh_info (T542).  egcs's own catch
 * code reaches these members by name through __start_cp_handler's return
 * type: a translation unit with a catch clause must see this complete
 * definition (a forward declaration of cp_eh_info breaks egcs there). */
struct cp_eh_info {
    __eh_info   eh_info;                        /* 0x00 match_function = __cplus_type_matcher, language 4, version 1 */
    void       *value;                          /* 0x08 the object (adjusted by a catch match) */
    void       *type;                           /* 0x0C type_info of the thrown type */
    cleanup_fn  cleanup;                        /* 0x10 destructor or NULL */
    bool        caught;                         /* 0x14 (egcs bool: 4 bytes, lw / sw) */
    cp_eh_info *next;                           /* 0x18 next exception in flight */
    s64         handlers;                       /* 0x20 `long`, 64-bit on the EE (ld / sd): active handlers */
    void       *original_value;                 /* 0x28 the object as allocated */
};
EGCS_SIZE(cp_eh_info, 0x30);

/* class exception - vtable _vt$9exception 0x003482B0 (2 slots), vptr +0x00
 * (no data members), size 0x4.  typeinfo __tf9exception 0x0027A3C0,
 * __user_type_info __ti9exception 0x00353510, RTTI name 0x003482D8.
 * Subclasses: bad_exception (here), bad_alloc (T550), bad_cast, bad_typeid
 * (T548). */
class exception {
public:
    exception();                                /* 0x0027A3A8 (inline in egcs's <exception>) */
    /* Modern build only: stands for egcs vtable entry 0 (the typeinfo
     * function itself, what typeid(*obj) calls); no address of its own. */
    virtual const type_info &__tf() const;
    virtual ~exception();                       /* [0] +0x0C 0x0027A378 (inline in <exception>) */
    virtual const char *what() const;           /* [1] +0x14 0x0027A2B8 typeid(*this).name() */
};

/* class bad_exception - vtable _vt$13bad_exception 0x00348298 (2 slots: [1]
 * is exception::what), size 0x4.  typeinfo __tf13bad_exception 0x0027A328,
 * __si_type_info 0x004B2B30, RTTI name 0x003482C8.  Thrown by
 * __check_eh_spec when the specification allows it. */
class bad_exception : public exception {
public:
    bad_exception();                            /* 0x0027A310 (inline in <exception>) */
    virtual const type_info &__tf() const;      /* modern build only, as above */
    virtual ~bad_exception();                   /* [0] +0x0C 0x0027A2E0 (inline in <exception>) */
};
EGCS_SIZE(exception, 0x4);
EGCS_SIZE(bad_exception, 0x4);

/* Default unexpected handler: std::terminate().  The initial value of
 * __unexpected_func (0x003064B4). */
void __default_unexpected();                    /* 0x00279CC0 */

extern "C" {
/* __start_cp_handler and __check_eh_spec: egcs declares both itself (C++
 * linkage, its own cp_eh_info) the first time it compiles a function with an
 * exception specification or a catch clause, and uses an earlier declaration
 * when there is one.  egcs's <new> (__NEW__) defines the inline placement
 * forms with throw(): after it, egcs has declared them and rejects these
 * declarations.  Without <new> they must stay: egcs's catch code then uses
 * this cp_eh_info (a TU with a catch clause and no declaration fails, "not a
 * base type"). */
/* malloc; std::terminate on failure. */
void *__eh_alloc(size_t size);                  /* 0x00279D88 */
/* `throw`: records the object (already in __eh_alloc memory). */
void  __cp_push_exception(void *value, void *type, cleanup_fn cleanup);    /* 0x00279E40 */
/* End of a catch clause: unlinks and destroys p once no handler uses it.
 * egcs declares it itself too (void *), at the same point as the two above;
 * this declaration after <new> makes it crash (internal compiler error at
 * the end of the next function with a throw() specification, u136). */
/* The catch-all handler of a throw(spec...) function (compiler-emitted; the
 * allowed types spec[0..n)): rethrows the exception in flight if it is
 * allowed, else std::unexpected(), then the same check on what that throws,
 * then bad_exception (if allowed) or std::terminate.  Never returns. */
/* Failed dynamic_cast to a reference / typeid of a null pointer. */
void  __throw_bad_cast(void);                   /* 0x0027A180 */
void  __throw_bad_typeid(void);                 /* 0x0027A200 */

}

#endif /* GAME_T543_EXCEPTION_H */
