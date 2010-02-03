#!/bin/sh

# bin_files are relative to $objdir/dist/bin/
bin_files="regxpcom*
           xpidl*
           xpt_dump*
           xpt_link*
           mozcrt19*
"

# lib_files are relative to $objdir/dist/lib/
lib_files="*js3250.*
           *plc4.*
           *plds4.*
           *nspr4.*
           *unicharutil_external_s.*
           *xpcom.*
           *xpcomglue_s.*
           *mozjs*
           *mozcrt19*
           *xul*
           *smime3*
           *ssl3*
           *nss3*
           *nssutil3*
           *sqlite3*
"

# symbol_files are relative to $objdir
symbol_files="intl/unicharutil/util/nsunicharutils.pdb
              intl/unicharutil/util/unicharutil_external_s.pdb
              js/src/js3250.pdb
              toolkit/library/xul.pdb
              xpcom/glue/nsarrayenumerator.pdb
              xpcom/glue/nsarrayutils.pdb
              xpcom/glue/nsautolock.pdb
              xpcom/glue/nscategorycache.pdb
              xpcom/glue/nscomarray.pdb
              xpcom/glue/nscomponentmanagerutils.pdb
              xpcom/glue/nscomptr.pdb
              xpcom/glue/nscrtglue.pdb
              xpcom/glue/nsenumeratorutils.pdb
              xpcom/glue/nsgenericfactory.pdb
              xpcom/glue/nsid.pdb
              xpcom/glue/nsiinterfacerequestorutils.pdb
              xpcom/glue/nsisupportsimpl.pdb
              xpcom/glue/nsmemory.pdb
              xpcom/glue/nsquicksort.pdb
              xpcom/glue/nsstringapi.pdb
              xpcom/glue/nstarray.pdb
              xpcom/glue/nsthashtable.pdb
              xpcom/glue/nsthreadutils.pdb
              xpcom/glue/nsvoidarray.pdb
              xpcom/glue/nsweakreference.pdb
              xpcom/glue/pldhash.pdb
              xpcom/glue/xpcomglue_s.pdb
              xpcom/stub/xpcom.pdb
              xulrunner/app/xulrunner.pdb
              xulrunner/stub/xulrunner-stub.pdb
              memory/jemalloc/mozcrt19*.pdb
              memory/jemalloc/libc*.pdb
"

# update_bin_files are relative to $objdir/dist/host/bin/
update_bin_files="*mar*
                  *bsdiff*
"

# update_script_files are relative to $srcdir/tools/update-packaging/
update_script_files="common.sh
                     make_full_update.sh
                     make_incremental_update.sh
                     unwrap_full_update.pl
"

# build_scripts are relative to $srcdir
build_script_files="build/cygwin-wrapper
                    build/autoconf/acoutput-fast.pl
                    build/autoconf/make-makefile
                    build/macosx/universal/fix-buildconfig
                    build/macosx/universal/unify
                    build/package/mac_osx/make-diskimage
                    build/package/mac_osx/mozilla.dsstore
                    config/build-list.pl
                    config/configobj.py
                    config/make-jars.pl
                    config/mozLock.pm
                    config/preprocessor.pl
                    config/printconfigsetting.py
"

# breakpad script files are relative to $srcdir
breakpad_script_files="toolkit/crashreporter/tools/symbolstore.py
                       toolkit/crashreporter/tools/upload_symbols.sh
"

# windows breakpad binary files are relative to $srcdir
breakpad_bin_files_win="toolkit/crashreporter/tools/win32/dump_syms.exe"

# other breakpad binary files are relative to $objdir/dist/host/bin/
breakpad_bin_files_nix="dump_syms"

notice() {
  echo $* 1>&2
}

if [ $# != 3 ]; then
  notice "usage: make-mozilla-sdk.sh [mozilla-src-dir] [mozilla-obj-dir] [songbird-sdk-dir]"
  exit 1
fi

MAKE=make
CP=cp
case `uname -s` in
  CYGWIN*|MINGW*)
    is_windows="1"
    ;;
  SunOS)
    MAKE=gmake
	if [ -x /usr/gnu/bin/cp ]; then
		CP=/usr/gnu/bin/cp
	fi
    ;;
esac

relsrcdir="$1"
temp1=`dirname "$relsrcdir"`
temp2=`basename "$relsrcdir"`
srcdir="`cd \"$temp1\" 2>/dev/null && pwd || echo \"$temp1\"`/$temp2"

relobjdir="$2"
temp1=`dirname "$relobjdir"`
temp2=`basename "$relobjdir"`
objdir="`cd \"$temp1\" 2>/dev/null && pwd || echo \"$temp1\"`/$temp2"

distdir="$objdir/dist"

relsdkdir="$3"
temp1=`dirname "$relsdkdir"`
temp2=`basename "$relsdkdir"`
sdkdir="`cd \"$temp1\" 2>/dev/null && pwd || echo \"$temp1\"`/$temp2"

mkdir -p "$sdkdir"

# build in bsdiff to enable partial update patches (binary diffs)
if test -d "$objdir"/other-licenses/bsdiff; then
  notice "making bsdiff..."
  cd "$objdir"/other-licenses/bsdiff && $MAKE
fi

notice "copying binary files..."
cd "$sdkdir" && mkdir -p bin
cd "$distdir/bin" && $CP -Lfp $bin_files "$sdkdir/bin"
cd "$distdir/host/bin" && $CP -Lfp $update_bin_files "$sdkdir/bin"

# breakpad binaries differ by platform
if test -n "$is_windows"; then
cd "$srcdir" && $CP -Lfp $breakpad_bin_files_win "$sdkdir/bin"
else
cd "$distdir/host/bin" && $CP -Lfp $breakpad_bin_files_nix "$sdkdir/bin"
fi

notice "copying library files..."
cd "$sdkdir" && mkdir -p lib
# some os don't have all these files, so silence errors
cd "$distdir/lib" && $CP -Lfp $lib_files "$sdkdir/lib" 2>/dev/null

notice "copying symbol files..."
# some os don't have these files, so silence errors
cd "$objdir" && $CP -Lfp $symbol_files "$sdkdir/lib" 2>/dev/null

notice "copying include files..."
cd "$sdkdir" && mkdir -p include
cd "$distdir/include" && $CP -RLfp * "$sdkdir/include"

notice "copying idl files..."
cd "$sdkdir" && mkdir -p idl
cd "$distdir/idl" && $CP -Lfp * "$sdkdir/idl"

notice "copying frozen sdk..."
cd "$sdkdir" && mkdir -p frozen
cd "$distdir/sdk" && $CP -RLfp * "$sdkdir/frozen"

notice "copying scripts..."
cd "$sdkdir" && mkdir -p scripts
cd "$srcdir" && $CP -Lfp $build_script_files "$sdkdir/scripts"
if test -d "$srcdir"/tools/update-packaging; then
  cd "$srcdir/tools/update-packaging" && $CP -Lfp $update_script_files "$sdkdir/scripts"
fi
cd "$srcdir" && $CP -Lfp $breakpad_script_files "$sdkdir/scripts"

notice "performing post-processing..."

# bump WINVER and _WIN32_WINNT to 0x501 on windows
if test -n "$is_windows"; then
cd "$sdkdir/include" && perl -p -i.bak -e 's/WINVER 0x500/WINVER 0x501/g;s/_WIN32_WINNT 0x500/_WIN32_WINNT 0x501/g' ./mozilla-config.h
cd "$sdkdir/include" && rm -f ./mozilla-config.h.bak
fi

notice "done."