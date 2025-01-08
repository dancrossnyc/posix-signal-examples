# Self-Pipe Trick Demonstration

This program demonstrates the "self-pipe trick"
(https://cr.yp.to/docs/selfpipe.html) for safely handling signal
delivery.  In particular, we show how this technique can be used
to safely reflect signal delivery into a broadcast on a
condition variable (which is not permitted in a signal handler).
