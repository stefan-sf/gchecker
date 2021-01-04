# Gulliver's Checker

*Caveat: The idea behind this project was and is to become familiar with Clang
and its ecosystem.  Thus don't expect a ready to use application.*

Gulliver's Checker is an attempt to automatically detect and warn about endian
dependent code.  For example, consider the following code snippet:

```c
int i = 0;
unsigned char *pc = (unsigned char *)&i;
*pc = 1;
```

On a little-endian machine `pc` points to the least significant byte whereas on
a big-endian machine `pc` points to the most significant byte of object `i`.
Therefore, the value of `i` depends on the machine's endianness which renders
the code non-portable.  GChecker aims to detect such cases and warn about:

```
$ gchecker test.c --
test.c:3:1: warning: Possible endian unsafe access of object 'i' of type 'int' through lvalue of type 'unsigned char'

    *pc = 1;
    ^
1 warning generated.
```

If you are interested in more details, then please have a look at
[this post](https://stefansf.de/post/endiana-jones/).



#### Build Requirements

```
dnf install clang-devel llvm-devel cmake ninja-build
```



#### Build

```
git clone https://github.com/stefan-sf/gchecker.git
mkdir gchecker/build
cd gchecker/build
cmake -G Ninja ..
ninja
```
