import assert from 'node:assert/strict';
import {Client,T,values,json,ints} from './wire.mjs';
import {directory,cleanup,start} from '../tests/network_helpers.mjs';
const executable=process.argv[2];if(!executable)throw Error('Usage: node tools/demo.mjs PATH_TO_EXCHANGE_SERVER');
const dir=directory();let server,buyer,seller;
try{
 server=await start(executable,dir,['--timeout-ms','30000']);buyer=await Client.connect(server.port,'127.0.0.1',30000);seller=await Client.connect(server.port,'127.0.0.1',30000);
 await buyer.login(1,'1'.repeat(32));await seller.login(2,'2'.repeat(32));
 console.log('Seller rests ten units at 100 ticks.');const rest=await seller.submit(1,1,100,2,100,10);assert.equal(rest.remaining,10n);assert.equal(rest.reservedInventory,10n);console.log(json(rest));
 console.log('Buyer crosses four at a limit of 105; execution uses the maker price of 100.');
 const original=await buyer.submit(1,1,200,1,105,4);assert.equal(original.filled,4n);assert.equal(original.cash,999999600n);assert.equal(original.inventory,100004n);console.log(json(original));
 buyer.close();await server.stop();seller.close();
 console.log('Restart the server and retry the same request.');server=await start(executable,dir,['--timeout-ms','30000']);buyer=await Client.connect(server.port,'127.0.0.1',30000);await buyer.login(1,'1'.repeat(32));
 const retry=await buyer.submit(1,1,200,1,105,4);if(json(retry)!==json(original))throw Error('retry changed outcome');console.log(json(retry));
 seller=await Client.connect(server.port,'127.0.0.1',30000);await seller.login(2,'2'.repeat(32));
 console.log('Seller kill switch cancels its remaining six units and releases the reservation.');const kill=await seller.submit(2,3);assert.equal(kill.status,'killed');assert.equal(kill.cancelled,1n);assert.equal(kill.reservedInventory,0n);assert.equal(kill.cash,1000000400n);assert.equal(kill.inventory,99996n);console.log(json(kill));
 console.log('Final book fields: changed, version, total, offset, count.');const book=values((await buyer.send(T.book,ints(0,0,10))).payload);assert.equal(book[2],0n);console.log(json(book));
 console.log('PASS deterministic settlement, restart, retry and kill-switch demo');
}finally{buyer?.close();seller?.close();await server?.stop();cleanup(dir);}
