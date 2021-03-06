# -*- Mode: Python -*-

def CheckCommand(context, cmd):
       context.Message('Checking for %s command... ' % cmd)
       result = WhereIs(cmd)
       context.Result(result is not None)
       return result

# Builders

strip_bld = Builder(action = 'strip -o $TARGET $SOURCE',
                    src_suffix = '.debug',
                    suffix = '')

# Construct freestanding environment

freestanding_env = Environment(BUILDERS = {'Strip' : strip_bld,
                                           })

freestanding_env['CFLAGS']  = "-Os -minline-all-stringops -m32 -march=pentium -mtune=core2 -pipe -g -std=gnu99 -ffreestanding -nostdlib -Wno-multichar -Wall"
freestanding_env['CFLAGS'] += " -ffunction-sections -fomit-frame-pointer -mregparm=3 "
freestanding_env['LINKFLAGS'] = "-m elf_i386 -gc-sections -N"
freestanding_env['LINK'] = "ld"
freestanding_env['AS'] = "yasm"
freestanding_env['ASFLAGS'] = "-g stabs -O5 -f elf32"

conf = Configure(freestanding_env, custom_tests = {'CheckCommand' : CheckCommand})

if not conf.CheckCommand("yasm"):
    print("Please install yasm.")
    Exit(1)

if not (conf.CheckCHeader("stdint.h") and
        conf.CheckCHeader("stdarg.h")):
    print("Standard C headers are missing.")
    Exit(1)

freestanding_env = conf.Finish()

def CheckPKGConfig(context, version):
     context.Message( 'Checking for pkg-config... ' )
     ret = context.TryAction('pkg-config --atleast-pkgconfig-version=%s' % version)[0]
     context.Result( ret )
     return ret

def CheckPKG(context, name):
     context.Message( 'Checking for %s... ' % name )
     ret = context.TryAction('pkg-config --exists \'%s\'' % name)[0]
     context.Result( ret )
     return ret

fw_env = Environment()

fw_env.ParseConfig('pkg-config --cflags --libs libraw1394')

fw_env['CPPPATH'] = ["#include/"]
fw_env['CCFLAGS'] = "-Os -march=native -pipe -g "
fw_env['CXXFLAGS'] = "-std=c++0x"
fw_env['CFLAGS'] = "-std=c99 "
fw_env['LIBS'] += ['slang']

conf = Configure(fw_env, custom_tests = {'CheckPKGConfig' : CheckPKGConfig,
                                       'CheckPKG' : CheckPKG })

if not conf.CheckPKGConfig('0.15.0'):
       print('pkg-config >= 0.15.0 not found.')
       Exit(1)

build_fw_scan = True
build_tools   = True;

if not conf.CheckPKG('libraw1394'):
       print('Could not find libraw1394')
       build_tools = False

if not conf.CheckCHeader('slang.h'):
       print('Could not find slang headers.')
       build_fw_scan = False

if not conf.CheckFunc('SLsmg_init_smg'):
       print('Could not link to slang.')
       build_fw_scan = False

if not build_fw_scan:
       print('fw_scan will not be built.')

if not build_tools:
       printf('Host tools will not be built.')

fw_env = conf.Finish()

Export('freestanding_env')
Export('fw_env')

SConscript(["standalone/SConscript"])

if build_tools:
       SConscript(["tools/SConscript"])

if build_fw_scan:
       SConscript(["fw_scan/SConscript"])

# EOF
