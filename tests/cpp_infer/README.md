# C++ inference storage boundary

The focused host gate compiles the production header with INFER enabled in both
allocation modes. Recording-only C fakes observe configuration, construction,
failure and exactly-once cleanup. They never follow an advertised arena span,
parse a model, allocate native resources or run SDK code. Initialized bytes are
only a fake model token. Multiple fake handles exercise move assignment without
claiming concurrent native session admission.

One single-TU specialization gives the embedded storage member an observable
destructor. Close reads only external flags and initialized external handle
tokens; it never accesses expired embedded storage. This safely checks call
order. Source and language lifetime rules separately establish why real native
cleanup must precede member destruction.

Custom returning hooks, custom hooks with NDEBUG, default NDEBUG and default
debug profiles are built. Default debug runs only cases that cannot trigger its
nonreturning assertion. The ordinary test checks stay enabled under NDEBUG.
Sanitizers instrument the whole fixture with ASan/UBSan and full leak checking.
Actual model or firmware runtime qualification is outside this fixture.
