There's so much to fix here.

Firstly, static initialisers for everything.
Needing to call init and then register is dumb.

Next, the pthread lock is dumb and I did it just to serialise logs,
but of course performance will suck.  My libdebug stuff in a different
repo is better here and I should really just import it and keep going.

