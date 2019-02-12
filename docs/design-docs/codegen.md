<!---
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

===============================================================================
Code Generation Interface
===============================================================================

The codegen directory houses code which is compiled with LLVM code generation
utilities. The point of code generation is to have code that is generated at
run time which is optimized to run on data specific to usage that can only be
described at run time. For instance, code which projects rows during a scan
relies on the types of the data stored in each of the columns, but these are
only determined by a run time schema. To alleviate this issue, a row projector
can be compiled with schema-specific machine code to run on the current rows.

Note the following classes, whose headers are LLVM-independent and thus intended
to be used by the rest of project without introducing additional dependencies:

CompilationManager (compilation_manager.h)
RowProjector (row_projector.h)

(Other classes also avoid LLVM headers, but they have little external use).

CompilationManager
------------------

The compilation manager takes care of asynchronous compilation tasks. It
accepts requests to compile new objects. If the requested object is already
cached, then the compiled object is returned. Otherwise, the compilation request
is enqueued and eventually carried out.

The manager can be accessed (and thus compiled code requests can be made)
by using the GetSingleton() method. Yes - there's a universal singleton for
compilation management. See the header for details.

The manager allows for waiting for all current compilations to finish, and can
register its metrics (which include code cache performance) upon request.

No cleanup is necessary for the CompilationManager. It registers a shutdown method
with the exit handler.

Generated objects
-----------------

* codegen::RowProjector - A row projector has the same interface as a
common::RowProjector, but supports a narrower scope of row types and arenas.
It does not allow its schema to be reset (indeed, that's the point of compiling
to a specific schema). The row projector's behavior is fully determined by
the base and projection schemas. As such, the compilation manager expects those
two items when retrieving a row projector.

================================================================================
Code Generation Implementation Details
================================================================================

Code generation works by creating what is essentially an assembly language
file for the desired object, then handing off that assembly to the LLVM
MCJIT compiler. The LLVM backend handles generating target-dependent machine
code. After code generation, the machine code, which is represented as a
shared object in memory, is dynamically linked to the invoking application
(i.e., this one), and the newly generated code becomes available.

Overview of LLVM-interfacing classes
------------------------------------

Most of the interfacing with LLVM is handled by the CodeGenerator
(code_generator.h) and ModuleBuilder (module_builder.h) classes. The CodeGenerator
takes care of setting up static intializations that LLVM is dependent on and
provides an interface which wraps around various calls to LLVM compilation
functions.

The ModuleBuilder takes care of the one-time construction of a module, which is
LLVM's unit of code. A module is its own namespace containing functions that
are compiled together. Currently, LLVM does not support having multiple
modules per execution engine so the code is coupled with an ExecutionEngine
instance which owns the generated code behind the scenes (the ExecutionEngine is
the LLVM class responsible for actual compilation and running of the dynamically
linked code). Note throughout the directory the execution engine is referred to
(actually typedef-ed as) a JITCodeOwner, because to every single class except
the ModuleBuilder that is all the execution engine is good for. Once the
destructor to a JITCodeOwner object is called, the associated data is deleted.

In turn, the ModuleBuilder provides a minimal interface to code-generating
classes (classes that accept data specific to a certain request and create the
LLVM IR - the assembly that was mentioned earlier - that is appropriate for
the specific data). The classes fill up the module with the desired assembly.

Sequence of operation
---------------------

The parts come together as follows (in the case that the code cache is empty).

1. External component requests some compiled object for certain runtime-
dependent data (e.g. a row projector for a base and projection schemas).
2. The CompilationManager accepts the request, but finds no such object
is cached.
3. The CompilationManager enqueues a request to compile said object to its
own threadpool, and responds with failure to the external component.
4. Eventually, a thread becomes available to take on the compilation task. The
task is dequeued and the CodeGenerator's compilation method for the request is
called.
5. The code generator checks that code generation is enabled, and makes a call
to the appropriate code-generating classes.
6. The classes rely on the ModuleBuilder to compile their code, after which
they return pointers to the requested functions.

Code-generating classes
-----------------------

As mentioned in steps (5) and (6), the code-generating classes are responsible
for generating the LLVM IR which is compiled at run time for whatever specific
requests the external components have.

The "code-generating classes" implement the JITWrapper (jit_wrapper.h) interface.
The base class requires an owning reference to a JITCodeOwner, intended to be the
owner of the JIT-compiled code that the JITWrapper derived class refers to.

On top of containing the JITCodeOwner and pointers to JIT-compiled functions,
the JITWrapper also provides methods which enable code caching. Caching compiled
code is essential because compilation times are prohibitively slow, so satisfying
any single request with freshly compiled code is not an option. As such, each
piece of compiled code should be associated with some run time determined data.

In the case of a row projector, this data is a pair of schemas, for the base
and the projection. In order to work for arbitrary types (so we do not need
multiple code caches for each different compiled object), the JITWrapper
implementation must be able to provide a byte string key encoding of its
associated data. This provides the key for the aforementioned cache. Similarly,
there should be a static method which allows encoding such a key without
generating a new instance (every time there is a request made to the manager,
the manager needs to generate the byte string key to look it up in the cache).

For instance, the JITWrapper for RowProjector code, RowProjectorFunctions, has
the following method:

static Status EncodeKey(const Schema& base, const Schema& proj,
                        faststring* out);

For any given input (pair of schemas), the JITWrapper generates a unique key
so that the cache can be looked up for the generated row projector in later
requests (the manager handles the cache lookups).

In order to keep one homogeneous cache of all the generated code, the keys
need to be unique across classes, which is difficult to maintain because the
encodings could conflict by accident. For this reason, a type identifier should
be prefixed to the beginning of every key. This identifier is an enum, with
values for each JITWrapper derived type, thus guaranteeing uniqueness between
classes.

Guide to creating new codegenned classes
----------------------------------------

To add new classes with code generation, one needs to generate the appropriate
JITWrapper and update the higher-level classes.

First, the inputs to code generation need to be established (henceforth referred
to as just "inputs").

1. Making a new JITWrapper

A new JITWrapper should derive from the JITWrapper class and expose a static
key-generation method which returns a key given the inputs for the class. To
satisfy the prefix condition, a new enum value must be added in
JITWrapper::JITWrapperType.

The JITWrapper derived class should have a creation method that generates
a shared reference to an instance of itself. The JITWrappers should only
be handled through shared references because this ensures that the code owner
within the class is kept alive exactly as long as references to code pointing with
it exist (the derived class is the only class that should contain members which
are pointers to the desired compiled functions for the given input).

The actual creation of the compiled code is perhaps the hardest part. See the
section below.

2. Updating top-level classes

On top of adding the new enum value in the JITWrapper enumeration, several other
top-level classes should provide the interfaces necessary to use the new
codegen class (the layer of interface classes enables separate components
of kudu to be independent of LLVM headers).

In the CodeGenerator, there should be a Compile...(inputs) function which
creates a scoped_refptr to the derived JITWrapper class by invoking the
class' creation method. Note that the CodeGenerator should also print
the appropriate LLVM disassembly if the flag is activated.

The compilation manager should likewise offer a Request...(inputs) function
that returns the requested compiled functions by looking up the cache for the
inputs by generating a key with the static encoding method mentioned above. If the
cache lookup fails, the manager should submit a new compilation request. The
cache hit metrics should be incremented appropriately.

Guide to code generation
------------------------

The resources at the bottom of this document provide a good reference for
LLVM IR. However, there should be little need to use much LLVM IR because the
majority of the LLVM code can be precompiled.

If you wish to execute certain functions A, B, or C based on the input data which
takes on values 1, 2, or 3, then do the following:

1. Write A, B, and C in an extern "C" namespace (to avoid name mangling) in
codegen/precompiled.cc.
2. When creating your derived JITWrapper class, create a ModuleBuilder. The
builder should load your functions A, B, and C automatically.
3. Create an LLVM IR function dependent on the inputs. I.e., if the input
for code generation is 1, then the desired function would be A. In that case,
request the module builder for a function called "A". The builder, when compiled,
will offer a pointer to the compiled function.

Note in the above example the only utility of code generation is avoiding
a couple of branches which decide on A, B, or C based on input data 1, 2, or 3.

Code generation gets much more mileage from constant propagation. To utilize this,
one needs to generate a new function in LLVM IR at run time which passes
arguments to the precompiled functions, with hopefully some relevant constants
based on the input data. When LLVM compiles the module, it will propagate those
constants, creating more efficient machine code.

To create a function in a module at run time, you need to use a
ModuleBuilder::LLVMBuilder. The builder emits LLVM IR dynamically. It is an
alias for the llvm::IRBuilder<> class, whose API is available in the links at
the bottom of this document. A worked example is available in row_projector.cc.

Useful resources
----------------
http://llvm.org/docs/doxygen/html/index.html
http://llvm.org/docs/tutorial/
http://llvm.org/docs/LangRef.html

Debugging
---------

Debug info is available by printing the generated code. See the flags declared
in code_generator.cc for further details.
