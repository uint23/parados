![logo](logo.png)

### parados
A simple home media server for UNIX systems.

### Build
```sh
make release # or debug
make install-conf
./parados -v
./parados
```

### Startup Scripts
Parados has platform specific init scripts to launch it. As
of now, there are: OpenBSD(rcctl), OpenRC(rc-service)
startup scripts.

```sh
make install-rcctl # Change to init platform script
```
> If you want to contribute more init scripts, feel free to
> do so; any help appreciated!

### Documentation
All documentation can be found in [docs/](docs/)
```sh
man 1 parados       # Parados server usage
man 7 parados       # Interfacing with parados
man 5 parados.conf  # Parados configuration
```

### Clients
In the parados Git repo, you can find two clients which use
the protocol.

First, _gorados_ is a Go based WebUI; it is
bare bones and if anyone would like to contribute, feel free
to do so!

Second, _shrados_ is a client written in POSIX shell. A list
of dependancies are in the [README](clients/shrados/README.md).
It behaves like a parados shell and is the one I peronally
use.

### Issues
If you find a bug or would like to suggest something,
open an issue on the GitHub page. Issues should be
detailed and give steps on replicating the issue if
possible.

### Contributions
If you wish to contribute, create a PR with details
on what is being changed, added or removed. **The code
style (indentation, variable names, formatting) should
_not_ be altered.** Please make sure you know how the
formatting of the project looks like before submitting
a PR.

> Note: it is recommended you don't expose parados itself
> but rather the client if using a web client. Also setting
> a HTTPS reverse proxy is a good idea

### Repository Links
[[GitHub](https://github.com/uint23/parados)] [[SourceHut](https://sr.ht/~uint/parados)]

