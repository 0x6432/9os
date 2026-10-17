# PR38 first current-head CI failure: actual strict-flag installer regression
Head609eb41, fast39062004730 x86 job117481714409 fails before Alpine guest
build/testing. Exact native unixpolltest216checks0fail and four benchmarkmodes
pass earlier in same job. Installer mock requires Werror/fortify forunixpolltest,
but general Alpine tiny-test loop did not supply them. Reproduced locally from
committed head (rc1). Preserve original full joblog and reproduction.
Fix supplies both flags only for new test, retaining every earlier consumer
and strictflag assertion; new separate missing-Werror and missing-fortify
mutants reject. Fixed installer model passes locally. No failed guest rerun,
no weakening model/gates. Earlier controls log preceded strictflag guard and
must not be cited as proof that the final committed installer guard passed.
Current-head CI must run anew after actual fix. M40/chain holds retained.
