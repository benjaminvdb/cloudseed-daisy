## What this changes

<!-- What behaviour differs afterwards, and why. -->

## Verification

<!-- Which suites were run, and on what. `bash test/all.sh /path/to/CloudSeed`
     runs every host suite; the fidelity suite needs the CloudSeed checkout. -->

- [ ] `test/all.sh` passes
- [ ] `test/consumer.sh` passes, if the build integration or the README's quick start changed
- [ ] `test/size.sh` passes, and `test/size_budget.txt` and the README's
      "Memory and flash" table are updated if a figure moved
- [ ] Tried on hardware, if the change can affect what the Seed does

## Numerical impact

<!-- Does output still match bit for bit between the staged and direct paths,
     and against the reference? If the arithmetic moved, say so: the port is
     kept expression for expression on purpose. -->
