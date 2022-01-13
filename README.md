# gimp_qoi_format

A small plug-in for GIMP that adds support for the Quite OK Image format (QOI).

## Building

This project uses scripts for building rather than make to not have deal with
the complexity of writing makefiles. Inside the folder named `scripts`, there
is a single script file for each platform that building is done for. Run that
script twice: once passing "build" as the first argument, once passing
"install" as the first argument. After that, everything should be built and
installed for you.

As GIMP provides two ways for plug-ins to be installed (system-wide or per
user), the build script has to make a choise. This project chooses to
install system-wide.

### Example

To build on linux you would position yourself at the root of the project in a
terminal and run the following two commands:

	./scripts/linux_build.sh build
	./scripts/linux_build.sh install

The plug-in should now be installed for the entire system and be ready to use
in GIMP.

## Used documentation

This is a list of the documentation used for this project, in case anyone wants
to work more on this plug-in or try and write one on their own.

* [QOI - The Quite OK Image Format](https://qoiformat.org)

	The original specification of QOI. The website links to a complete spec, a
	reference en-/decoder, and test images.

* [GIMP Reference Manuals](https://developer.gimp.org/api/2.0/index.html)
* [GIMP Developer Resources](https://developer.gimp.org)

	This website was discovered after the plug-in was written and therefor was
	not used. It would have been a usefull resource for developing though and
	perhaps it will help other people if they want to write their own plug-ins
	for GIMP.

* [Gtk 3.0](https://docs.gtk.org/gtk3/index.html)
* [Glib 2.0](https://docs.gtk.org/glib/index.html)
* [GEGL](https://gegl.org)

	Finding proper documentation was difficult and therefore source code
	comments (those intended to be used for generating the documentation) were
	used instead.

* [babl](https://www.gegl.org/babl/index.html)
* [Hacking: How to write a GIMP plug-in](https://www.wiki.gimp.org/wiki/Hacking:How_to_write_a_GIMP_plug-in)

	This was used to get a general idea of how GIMP plug-ins work.

## License

This is free and unencumbered software released into the public domain.

For more information, please refer to <https://unlicense.org>
