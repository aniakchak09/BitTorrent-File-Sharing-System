# BitTorrent-like File Sharing System

A distributed file sharing system using MPI and threads that mimics BitTorrent's peer-to-peer architecture.

## Components
### Tracker (rank 0)

- Maintains list of files in system
- Tracks which clients are seeds/peers for each file
- Handles peer registration and file information
- Processes update requests and network state changes

### Peers (rank > 0)

- Run concurrent download and upload threads
- Download missing chunks randomly from seeds/peers, to distribute load evenly
- Act as seeds for complete files they own
- Update network view every 10 downloaded chunks
- Write completed downloads to disk as clientX_fileY

### Communication

- DOWNLOAD_TAG (1): Request file chunks
- UPLOAD_TAG (2): Response with chunk data
- UPDATE_TAG (3): Network state updates

### Data Structures

- FileData: Stores file info and chunk hashes
- Args: Contains peer state for threads
- Seeds/Peers matrices: Track file ownership

### Other notes
I believe that and implementation where, when a peer finishes downloading a file, the tracker notices all the other clients about the update in the peers/seeds
lists would be more efficient than the current implementation. The update sent by the tracker would be received by a listener thread of the clients. I did not implement this version because the homework statement mentioned updating the lists every 10 downloaded chunks.