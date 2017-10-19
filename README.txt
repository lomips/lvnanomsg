================================================================================
                               NANOMSG for LABVIEW
================================================================================
        created and maintained by Chen Yucong, slaoub@gmail.com
================================================================================

NANOMSG is a great procotol for communication using a variety of
paradigms, providing easy solutions for many-many data sharing (e.g. publishing,
subscribing and routing).

LabVIEW-NANOMSG (LVNANOMSG) provides enables LabVIEW users to take advantage of these
techniques.

Installation is handled by the VI Package Manager (VIPM) available from
http://jkisoft.com/vipm/ (the free community version works fine).

Simply open the most recent VIP file in the package manager and click "install".

Further platform-specific instructions are below.


***** WINDOWS INSTALLATION *****


***** LINUX INSTALLATION *****

1. Install zeromq in the usual way for your distribution. Most distributions
   will provide some kind of precompiled package, otherwise obtain the source
   from http://www.nanomsg.org

2. Install the VI package with VIPM. The package should end up installed in the
   LabVIEW directory <vi.lib>/addons/nanomsg
     (e.g. /usr/local/natinst/LabVIEW-2010/vi.lib/addons/nanomsg)

3. Navigate to the "lib" directory of the package install location and modify
   the "makefile" to contain the directory of your LabVIEW installation.
   Execute "make".
 
This will compile the shared library object for your host, overwriting the
binary distributed in the package. You will have to remake the library every
time you upgrade the package.
