# Interactive netlab exercises

Place each implemented instructional lab under its curriculum number, for
example `netlabs/labs/101/`, with `topology.yml` and supporting configuration
and learning materials. The catalog in `netlabs/README.md` is a curriculum
proposal and does not by itself make a lab deployable.

```sh
netlabs/tools/remote-lab.sh --lab 101
netlabs/tools/remote-lab.sh --status
netlabs/tools/remote-lab.sh --connect
netlabs/tools/remote-lab.sh --stop
```

Unlike automated `netlabs/uut/` tests, instructional labs stay running until
explicitly stopped. Shared host-side session exclusivity is still enforced.
