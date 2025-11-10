# CS564_P3

## Buf Stats

When testing, I found out that the tests don't look at bufStats. These would be things like accesses, diskReads, and diskWrites.
  - diskReads and diskWrites are self explanatory
  - I am confused about the accesses. By accesses to the buffer, does this include the hastable and the buf table(buf descriptors). Also, what about when we are just returning a pointer to the frame in the bufPool. Does that count as an access?

