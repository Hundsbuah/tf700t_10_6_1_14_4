exfat-nofuse
============

Linux non-fuse read/write kernel driver for the exFAT file system.<br />
Originally ported from android kernel v3.0.


Kudos to ksv1986 for the mutex patch!<br />
Thanks to JackNorris for being awesome and providing the clear_inode() patch.<br />
<br />
Big thanks to lqs for completing the driver!


Special thanks to github user AndreiLux for spreading the word about the leak!<br />


Installation:
> make<br />
> make install

To load the driver manually, run this as root:
> modprobe exfat


Free Software for the Free Minds!
