# Parallel and Distributed Algorithms Project

The program simulates, using ***MPI***, the logic of the BitTorrent protocol. Thus, we have clients (peers) and a tracker.

Peer → holds segments of a file it is interested in, so it deals with both uploading and downloading that file (shares the segments it holds, receives segments it lacks from other clients).

Tracker → acts as an intermediary; it does not hold any segments of any file and does not contribute to the sharing process, but only maintains updated lists of the swarms responsible for files and the locations of their segments.
