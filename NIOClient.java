
/**
 * 
 */
package net.byted.sys.sandbox;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Iterator;
import java.util.List;
import java.util.Set;

class Session {
	int nread = 0;
	int bufEndPtr = 0;
	long lastActive;
	SocketChannel sc;
	
	String fileName = null;
	OutputStream writer = null;
	byte[] buf = null;
	static final int DEFLEN = 256*1024;
	

	Session(String ip) {
		nread = 0;
		buf = new byte[DEFLEN];
		
		//String fileName = String.format("GMetrics_%s_%d.log", iaddr.getAddress().getHostAddress(), lastActive);
		fileName = String.format("GMetrics.%s.%s.log.gz",
				new SimpleDateFormat("yyyyMMdd").format(new Date()), ip);
	}

	void add(long n) {
		nread += n;
	};
	
	void add(ByteBuffer sbuf, int len) {
		if(len + bufEndPtr > DEFLEN) {
			flushToDisk();
		}
		// assert sbuf.length <= DEFLEN;
		System.arraycopy(sbuf.array(), 0, buf, bufEndPtr, len);
		nread += len;
		bufEndPtr += len;
	}
	
	void flushToDisk() {
		if(nread < 1) {
			return;
		}
		try {
			if(writer == null) {
				writer = new BufferedOutputStream(new FileOutputStream(fileName, true));
			}
			writer.write(buf, 0, bufEndPtr);
			bufEndPtr = 0;
		} catch (Exception e) {
			e.printStackTrace();
		}
	}
	
	void close() {
		System.out.printf("XXX Write %d data to file %s, closing ...\n"
				, nread, fileName);
		
		if(writer == null) {
			return;
		}
		
		flushToDisk();
		try {
			writer.flush();
		} catch (IOException e) {
			// TODO Auto-generated catch block
			e.printStackTrace();
		}
	}


	/*
	 
	protected void finalize() {
		System.out.printf("Write %d data to file %s, closing ...\n", nread, fileName);
	}
	
	protected void finalize() {
		if(bufEndPtr != 0) {
			flushToDisk();
		}
		if(writer != null) {
			try {
				writer.close();
			} catch (IOException e) {
				e.printStackTrace();
			}
		}
	}
	
	*/
}


class WorkerThreads extends Thread {
	// List<SocketChannel> openChannels = null;
	List<String> ips = null;
	List<Session> aliveSession = null;
	int tid = -1;
	Selector selector = null;
	
	int parallelNFD = 2048;
	int BUFFER_SIZE = 32768;
	
	int timeout = 500;
	int selTimeout = 500;

	int connected = 0;
	int closed = 0;

	public WorkerThreads() throws IOException {
		super();
		// openChannels = new ArrayList<SocketChannel>();
		ips = new ArrayList<String>();
		aliveSession = new ArrayList<Session>();
		selector = Selector.open();
	}

	public WorkerThreads(List<String> ips, int tid) throws IOException {
		// openChannels = new ArrayList<SocketChannel>();
		this.ips = ips;
		this.tid = tid;
		aliveSession = new ArrayList<Session>();
		selector = Selector.open();
	}

	public int aliveSize() {
		return connected - closed;
	}

	public boolean doConnect(String ip, int port) {

		SocketChannel myCliCh = null;
		try {
			InetSocketAddress myAddress = new InetSocketAddress(ip, port);
			myCliCh = SocketChannel.open();
			myCliCh.configureBlocking(false);
			
			Session s = new Session(ip);

			//myCliCh.register(selector, 
			//		SelectionKey.OP_CONNECT|SelectionKey.OP_READ|SelectionKey.OP_WRITE, null);
			myCliCh.register(selector, SelectionKey.OP_CONNECT|SelectionKey.OP_READ, s);
			myCliCh.connect(myAddress);
			s.lastActive = System.currentTimeMillis();
			s.sc = myCliCh;
			aliveSession.add(s);

			return true;

		} catch (IOException e) {
			e.printStackTrace();
			if (myCliCh != null && myCliCh.isConnected()) {
				try {
					myCliCh.close();
				} catch (IOException e1) {
					e1.printStackTrace();
				}

				return false;
			}

		}
		// make compiler happy
		return false;
	}
	
	
	// TODO
	public void close(Session e) {
		;
	}

	public void run() {

		int i = 0;

		while ((connected < parallelNFD) && (i < ips.size())) {
			if (doConnect(ips.get(i), 8649)) {
				connected++;
				System.out.println("Connected " + i + " " + ips.get(i));
			}
			i++;
		}

		while (true) {
			if ((closed == connected) && (i >= ips.size())) {
				break;
			}

			long now = System.currentTimeMillis();
			
			// consume
			int num = 0;
			try {
				num = selector.select(selTimeout);
			} catch (IOException e) {
				e.printStackTrace();
			}
			
			if (num > 0) {
				Set<SelectionKey> set = selector.selectedKeys();
				
				//System.out.println(num + " = " + set.size());
				
				Iterator<SelectionKey> it = set.iterator();
				while (it.hasNext()) {
					SelectionKey sk = it.next();
					Session s = (Session)sk.attachment();

					// In the select and delete delete next selection, if not it will still exist.
					it.remove();
					
					SocketChannel ssc = (SocketChannel) sk.channel();
					SocketAddress raddr = null;

					
					try {
						raddr = ssc.getRemoteAddress();

						if(sk.isConnectable()) {
							System.out.println(num + ": C " + raddr.toString());
							ssc.finishConnect();
						}
						
						// never called
						if(sk.isWritable()) {
							System.out.println(num + ": W " + raddr.toString());
						}
	
						if (sk.isReadable()) {
	
							ByteBuffer myBuffer = ByteBuffer.allocate(BUFFER_SIZE);
							int nbytes = 0;
							try {
								nbytes = ssc.read(myBuffer);
							} catch (IOException e) {
								e.printStackTrace();
							}
									
							//System.out.println(num + ": R " + nbytes +
							//		" bytes from " + raddr.toString());
							

							// If you want to be notified when the peer closes the connection,
							// OP_READ will fire and a read will return -1.			
							if(nbytes == -1) {
								s.close();
								aliveSession.remove(s);
								sk.attach(null);
								
								sk.cancel();
								ssc.close();
								closed++;
								
								System.out.println("Read totally " + s.nread + 
										", close connection: " + raddr.toString());
								continue;
							}
							
							// non -1
							//s.add(nbytes);
							s.add(myBuffer, nbytes);
							s.lastActive = now;
						}
					} catch (Exception e) {
						
						if(s.nread > 0) {
							s.close();
						}
						aliveSession.remove(s);
						sk.attach(null);
						
						sk.cancel();
						try {
							ssc.close();
						} catch (IOException e1) {
							// TODO Auto-generated catch block
							e1.printStackTrace();
						}
						closed++;
						
						if(e instanceof java.net.ConnectException) {
							System.out.printf("Error Connect to %s\n", raddr.toString());
						} else {
							e.printStackTrace();
						}
					}

					//System.out.println(num + ": R " + sk.isReadable() + " W " + sk.isWritable());
				}
			}
			
			if(num == 0) {
				// check any timeout
				Iterator<Session> it = aliveSession.iterator();
				while(it.hasNext()) {
					Session s = it.next();

					if((now - s.lastActive) > timeout) {
						SocketChannel sc = s.sc;
						try {
							SocketAddress raddr = sc.getRemoteAddress();
							sc.close();

							closed++;
							it.remove();
							
							System.out.println("Read totally " + s.nread + 
									", timeout " + timeout +" ms, close connection: " + raddr.toString());
						} catch (IOException e1) {
							// TODO Auto-generated catch block
							e1.printStackTrace();
						}
					}
				}
			}


			while (((connected - closed) < parallelNFD) && (i < ips.size())) {
				System.out.println("Connected " + connected
						+ " Closed " + closed + ", i=" + i);	
				if (doConnect(ips.get(i), 8649)) {
					connected++;
					System.out.println("Connected " + i + " " + ips.get(i));
				}
				i++;
			}
		}
	}
}

/**
 * @author shine
 *
 */
public class NIOClient {

	private static final int BUFFER_SIZE = 1024;
	List<Thread> threads = null;
	List<String> ips = null;

	/**
	 * 
	 */
	public NIOClient() {
		// TODO Auto-generated constructor stub
		threads = new ArrayList<Thread>();
		ips = new ArrayList<String>();
	}

	public static void logger(String msg) {
		System.out.println(msg);
	}

	public void fire(List<String> ipList) {
		logger("Starting MySelectorClientExample...");
		int port = 8649;
		for (String ip : ipList) {
			;
		}
	}

	public void getIPList(String fileName) {
		String line = null;
		try (BufferedReader br = new BufferedReader(new FileReader(fileName))) {
			while ((line = br.readLine()) != null) {
				// process the line.
				if (line.length() < 7) {
					// wrong lines
					continue;
				}
				ips.add(line);
			}
		} catch (Exception e) {
			System.err.println("Line: " + line);
			e.printStackTrace();
		}
	}

	/**
	 * @param args
	 */
	public static void main(String[] args) {
		// TODO Auto-generated method stub

		int nThreads = 1;
		if (args.length > 0) {
			nThreads = Integer.parseInt(args[0]);
			System.out.println("Thread Number Set to " + nThreads);
		}

		NIOClient me = new NIOClient();
		me.getIPList("/Users/shine/Documents/GmetadList.txt");

		int nsegs = (me.ips.size() + nThreads - 1) / nThreads;
		int offset = 0;
		for (int i = 0; i < nThreads; i++) {
			if ((offset + nsegs) > me.ips.size()) {
				nsegs = me.ips.size() - offset;
			}
			List<String> subset = me.ips.subList(offset, offset + nsegs);
			offset += nsegs;
			WorkerThreads t = null;
			try {
				t = new WorkerThreads(subset, i);
			} catch (IOException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			}
			t.start();
		}

	}

}
