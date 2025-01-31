# Guess the Number Game Server

A high-performance TCP server implementing a multiplayer "Guess the Number" game using non-blocking I/O and efficient connection management.

## Features
- 🎮 Multiplayer guessing game with real-time broadcasts  
- 🔒 Strict player limit enforcement with waiting queue  
- 📡 Non-blocking I/O using `select()` system call  
- 🔄 Automatic game restart after correct guess  
- 📊 Detailed server logging without busy waiting  
- 🛠 Robust error handling and resource cleanup  

## Installation
```bash
# Compile with GCC (requires C99 standard)
gcc -std=c99 -Wall -o server gameServer.c

# Verify compilation
./server --version || echo "Server compiled successfully"
```

## Usage
```bash
# Start server with parameters:
./server <port> <seed> <max-players>

# Example (port 8060, seed 42, max 3 players):
./server 8060 42 3
```

## Game Protocol
**Client Messages**  
- Send numeric guesses as plain text (e.g., `50\n`)  

**Server Responses**  
```
Welcome to the game, your id is <ID>
Player <ID> guessed <X>
The guess <X> is too [high|low]
Player <ID> wins
The correct guessing is <X>
Player <ID> [joined|disconnected]
```

## Testing Guide
### Basic Gameplay
```bash
# Terminal 1 (Server)
./server 8060 42 2

# Terminal 2 (Player 1)
telnet localhost 8060
> Welcome to the game, your id is 1
> 50
< Player 1 guessed 50
< The guess 50 is too low

# Terminal 3 (Player 2)
telnet localhost 8060
> Player 1 guessed 50
> Welcome to the game, your id is 2
> 75
< Player 2 guessed 75
< The guess 75 is too high
```

### Advanced Scenarios
**1. Max Player Handling**  
```bash
# With max-players=2:
# - First 2 connections become active players
# - 3rd connection waits silently until slot opens
```

**2. Win Condition**  
```bash
Correct guess triggers:
1. Immediate broadcast of win messages
2. All connections closed
3. New random number generated
4. Waiting players can join fresh game
```

**3. Disconnection Handling**  
```bash
# Player disconnection:
- Active players see "Player <ID> disconnected"
- First waiting player automatically joins
```

## Technical Specifications
**System Requirements**  
- POSIX-compliant OS (Linux/macOS)  
- GCC compiler (minimum v9.4.0)  
- Basic network utilities (telnet/netcat)  

**Key Implementations**  
1. **I/O Multiplexing**  
   - Single `select()` call manages all sockets  
   - Write queues prevent blocking on partial sends  

2. **Connection Lifecycle**  
   ```mermaid
   graph TD
     A[New Connection] --> B{Slots Available?}
     B -->|Yes| C[Assign ID, Start Game]
     B -->|No| D[Add to Wait Queue]
     C --> E[Game Interaction]
     D --> F[Monitor for Disconnects]
     E --> G{Guess Correct?}
     G -->|Yes| H[Reset Game]
     G -->|No| E
   ```

3. **Data Structures**  
   - Client array with write queues  
   - Wait queue for pending connections  
   - Available ID stack for fast reuse  

## Troubleshooting
**Common Issues**  
1. **Connection Refused**  
   ```bash
   # Check server status and port permissions:
   sudo lsof -i :8060
   ```

2. **Busy Waiting Logs**  
   - Verify `select()` return value handling  
   - Ensure write FDs only set when queue non-empty  

3. **Telnet Commands**  
   ```bash
   # To disconnect properly in telnet:
   Ctrl+] → quit
   ```

## License
MIT License - See [LICENSE](LICENSE) for details