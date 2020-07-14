#include <oig.hpp>

using namespace oigspace;

/*  
 *  
 *  Contract states:  elect.state ==  10  - contract not initialized
 *                    elect.state ==  00  - contract clean
 *                    elect.state ==  01  - election created
 *                    elect.state ==  02  - nomination in progress
 *                    elect.state ==  03  - nomination closed
 *                    elect.state ==  04  - voting in progress
 *                    elect.state ==  05  - voting commenced
 *                    elect.state ==  06  - cleanup initiated
 * 
 * * * */

/*
 * init():  Registers the contract as voter on decide to allow it to create ballots.
 *          Sets the contract state to clean.
 * authorisation: admin
 * requirements:
 *    Decide needs to be initialized. 
 *    The main treasury (8,VOTE) created and managed by get_self().
 *    To prevent 'doublespending' of votes the treasury needs to be private.
 *    eosio.code permission
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::init(){
  require_auth( get_self() );
  // create the election singleton that tracks election states and registered voters
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get_or_create(get_self());
  // initi fails if ran more than once
  check(elect.state == 10, "Contract already initialized.");
  // registering the contract as voter with decide
  RegVoter args;
  args.voter = get_self();
  permission_level permissionLevel = permission_level(get_self(), name("active"));
  action transferAction = action(
      permissionLevel,
      name("decide"),
      name("regvoter"),
      std::move(args)
  );
  transferAction.send();
  
  elect.state = 0; // setting election state to clean
  elections.set(elect, get_self());
}


/*
 * inaugurate() initializes or cancels the election process
 * 
 * authorisation: admin
 * requirements:
 *    For creation the contract needs to be in a clean state: elect.state == 0
 *    The contract needs to hold 30 WAX to pay the ballot creation fee.
 *    Elections can be cancelled until the ballot is created: elect.state <= 2
 *    Cancelling an election sets it into cleanup state. To clean the election call
 *    updtstate(). Depending on voter count updtstate() might need to be called multiple times.
 * 
 * arguments:
 *    string title: Election title  string description: Election description
 *    string content:  Additional information IPFS link or URL
 *    time_point_sec nmn_open, nmn_close:   Nomination window.
 *    time_point_sec vote_open, vote_close: Voting window.
 *      Timestamps need to be provided in UTC as:
 *      YYYY-MM-DDTHH:MM:SS - 2020-12-31T00:00:00
 *    boolean cancel: elections can be cancelled during nomination
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::inaugurate( string title, string description, string content, time_point_sec nmn_open, 
                        time_point_sec nmn_close, time_point_sec vote_open, time_point_sec vote_close, bool cancel) {
  // authorize
  require_auth( get_self() );
  // initialize
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get_or_default();
  // check contract state
  check(elect.state != 10, "Contract not initialized.");

  if (cancel) {
    // cancellation is limited to the setup and nomination phase to prevent abandoned ballots in decide
    check(elect.state <= 2, "Can't cancel once ballot is created.");
    check(elect.state != 0, "No Election running.");
    uint64_t tmp_ballot = elect.ballot.value;
    tmp_ballot--; // resetting the ballot key
    elect.ballot = name(tmp_ballot);
    elect.state = 6; // setting to cleanup state
    elections.set(elect, get_self());

  } else {
    check(elect.state != 5 , "Cleanup required."); // if in state 5, run endelection() to initialize cleaning
    check(elect.state != 6 , "Cleanup in progress."); // if stuck in 6, run updstate(), updstate() calls cleanup
    check(elect.state == 0 , "Election already running.");
    check(!title.empty(), "title required");
    check(!description.empty(), "description required");
    auto now = time_point_sec(current_time_point());
    check(now <= nmn_open, "dates need to be in the future");
    check(nmn_open < nmn_close, "nomination duration needs to be positive");
    check(nmn_close < vote_open, "voting period can't overlap with nomination period");
    check(vote_open < vote_close, "voting duration needs to be positive");

    uint64_t tmp_ballot = elect.ballot.value;
    tmp_ballot++; // setting the ballot primary key

    elect.ballot      = name(tmp_ballot);
    elect.state       = 1;
    elect.title       = title;
    elect.description = description;
    elect.content     = content;
    elect.nmn_open    = nmn_open;
    elect.nmn_close   = nmn_close;
    elect.vote_open   = vote_open;
    elect.vote_close  = vote_close;
    elections.set(elect, get_self());
  }
}


//========== nomination methods ==========

/*
 * nominate() Allows one account to nominate itself or someone else.
 *            Self nominations are automatically accepted.
 * 
 * authorisation: nominator
 * requirements:
 *    nomination needs to be in progress (elect.state == 1 || 2)
 *    To cope with possible nomination spam are limited to 200.
 *    If nominations ever exceed 200 the contract will remove all unaccepted nominations.
 *    If nomintations exceed 150 self nominations are not accepted automatically anymore.
 * 
 * arguments:
 *    name nominator:   account signing the transaction
 *    name nominee:     account to be nominated
 * 
 * TODO:
 *    Eliminate nominations cap. 
 *    Change scope of nomination to nominee.
 *    Write cleanup for new nomination structure.
 *    Incorporate into front-end.
 *    Implement a nomination fee. (?)
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::nominate(name nominator, name nominee) {
  //authenticate
  require_auth( nominator );
  //initialize
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  //verify
  check(elect.state != 0, "No election is currently running.");
  check(elect.state <= 2, "Nomination period has already closed.");
  //get data
  nominations_table nominations(get_self(), get_self().value);
  auto nmne_itr = nominations.find(nominee.value);
  //verify
  check(nmne_itr == nominations.end(), "Nomination already exists.");
  check(is_account(nominee), "Nominated account must exist.");

  // dirty spam prevention
  uint8_t count = elect.nom_count;
  if (count > 200) {
    auto nomn = nominations.begin();
    while ( nomn != nominations.end()) {
      if (!nomn->accepted) {
        nomn = nominations.erase(nomn);
        count--;
      } else {
        ++nomn;
      }
    }
    elect.nom_count = count;
  }
  // auto-accept self nominations
  bool accepted = 0;
  if (nominator == nominee && count < 150) {
    accepted = 1;
  }
  //emplace new nominee
  nominations.emplace(get_self(), [&](auto& col) {
    col.nominee = nominee;
    col.accepted = accepted;
  });
  // track nominations count
  elect.nom_count = ++count;
  elections.set(elect, get_self());
  state_refresh(); // call state_refresh() to progress in election if needed
}

/*
 * proclaim() Called to accept or decline a nomination.
 *            Declining a nomintation will delete it.
 * 
 * authorisation: nominee
 * requirements:  
 *    Nomination needs to be in progress (elect.state == 1 || 2)
 *    nominee account needs to exist.
 *    nominee must not be nominated already.
 * 
 * arguments:
 *    name nominee:   nomination in question
 *    bool decision:  accepted || declined
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::proclaim(name nominee, bool decision) {
  // authenticate
  require_auth( nominee );
  // initialize
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  // verify
  check(elect.state != 0, "No election is currently running.");
  check(elect.state <= 2, "Nomination period has alreaedy closed.");
  // get data
  nominations_table nominations(get_self(), get_self().value);
  auto& nmne = nominations.get(nominee.value, "Nomination not found.");

  if (decision) {
    nominations.modify(nmne, nominee, [&](auto& col) {
        col.accepted = decision;
        printf("Nomination accepted!");
    });
  } else { // erase nomination if declined
    nominations.erase(nmne);
    printf("Nomination declined!");
    elect.nom_count = --elect.nom_count;
    elections.set(elect, get_self());
    // doublecheck if the nominee has already given info and delete if needed.
    nominees_table nominees(get_self(), get_self().value);
    auto desc = nominees.find(nominee.value);
    if (desc != nominees.end()) {
        nominees.erase(desc);
    }
  }
  state_refresh(); // call state_refresh() to progress in election if needed
}

/*
 * nominf() Allows nominees to provide personal info or delete given info.
 * 
 * authorisation: nominee
 * requirements:
 *    Voting not yet in progress. (elect.state <= 3)
 *    Nomination needs to exist and be accepted.
 *      
 * arguments:
 *    name nominee: nominee account     string name: Plain text name. cap 99 chars
 *    string descriptor: candidates representation. cap 2000 chars
 *    string picture: url to a picture. cap 256 chars
 *    string telegram, twitter, wechat: social meadia contacts. cap 99 chars each.
 * 
 * TODO: check if there is a combined modify or emplace function.
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::nominf(name nominee, string name, string descriptor, string picture, string telegram, string twitter, string wechat, bool remove) {
    // authenticate
    require_auth( nominee );
    // initilize
    election_singleton elections(get_self(), get_self().value);
    auto elect = elections.get();
    // verify 
    check(elect.state <= 3, "Voting has already commenced.");

    //doublecheck if the nominee exists and accepted
    nominations_table nominations(get_self(), get_self().value);
    auto nmne_itr = nominations.require_find(nominee.value, "Account not nominated.");
    check(nmne_itr->accepted, "Nomination not accepted.");
    // fetching possible existing entry
    nominees_table nominees(get_self(), get_self().value);
    auto nmne = nominees.find(nominee.value);

    if (remove) { // if 'remove' just skip everything and delete the record.
        check(nmne != nominees.end(), "can't delete non-existing record");
        nominees.erase(nmne);
    } else {
      // validate
      check(!name.empty(), "name required");
      check(name.length() <= 99, "name too long");
      check(descriptor.length() <= 2000, "description too long");
      check(picture.length() <= 256, "picture too long");
      if (!picture.empty()) check(picture.substr(0, 4) == "http", "picture should begin with http");
      check(telegram.length() <= 99, "telegram too long");
      check(twitter.length() <= 99, "twitter too long");
      check(wechat.length() <= 99, "wechat too long");
      // create if new entry
      if (nmne == nominees.end()) {
         nominees.emplace(nominee, [&](auto& col) {
            col.owner = nominee;
            col.name = name;
            col.descriptor = descriptor;
            col.picture = picture;
            col.telegram = telegram;
            col.twitter = twitter;
            col.wechat = wechat;
         });
      } else { // modify if record exists
         nominees.modify(nmne, nominee, [&](auto& col) {
            col.owner = nominee;
            col.name = name;
            col.descriptor = descriptor;
            col.picture = picture;
            col.telegram = telegram;
            col.twitter = twitter;
            col.wechat = wechat;
         });
      }
   }
   state_refresh(); // call state_refresh() to progress in election if needed
}

/*
 * updtstate()  Calls refresh_state() to progress in election if needed
 * 
 * authorisation: none
 * requirements: none
 * arguments: none
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::updtstate() {
  state_refresh();
}

/*
 * regvoter()   Registers accounts as voters with decide in the (8,Vote) treasury if needed.
 *              Places a 'flag' for the account to keep track of it being registered.
 *              Adds the voter to one of the according tracking vectors, 
 *              to have their balances synced preventing 'doublespending' of votes if needed.
 * 
 * authorisation: voter
 * requirements:
 *    Account needs to exist.
 *    (8,VOTE) treasury needs to be created, private and managed by get_self().
 * 
 * arguments:
 *    name voter: account to be registered as voter.
 * 
 * TODO:
 *    Accomodate 3rd party contracts to use the (8,VOTE) treasury.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::regvoter(name voter) {
  // authorize
  require_auth( voter );
  // validate
  check(is_account(voter), "Voter account must exist.");
  // initialize
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  // load table
  reggedvoters_table reggedvoters(get_self(), voter.value);
  // check if voter is already regged and skip further execution if needed
  auto voter_itr = reggedvoters.find(get_self().value);
  if (voter_itr == reggedvoters.end()) { // place a flag for new voters
    reggedvoters.emplace(get_self(), [&](auto& col) { 
      col.referrer = get_self();
      col.treasury = VOTE_SYM;
      col.voter = voter;
    });
    // set permission level for inline actions
    permission_level permissionLevel = permission_level(get_self(), name("active"));
    RegVoter reg;
    reg.voter = voter; // prepare arguments for inline action
    action regAction = action(
      permissionLevel,
      name("decide"), // contract to call
      name("regvoter"), // function to call
      std::move(reg));
    regAction.send();
    // based on vector state add the new voter to one of the trackers
    if(!elect.synced_voter.empty()) {
      if (elect.voter.size() != 0 ) {
        elect.voter.push_back(voter);
      } else {
        elect.synced_voter.push_back(voter);
      }
    } else {
      elect.voter.push_back(voter);
    }
    // write changes back to the election record
    elections.set(elect, get_self());
  }
}

/*
 * endelection() Sets an ended election into cleanup state and starts the cleanup process.
 * 
 * authorisation: admin
 * requirements:  Voting needs to have concluded. (elect.state == 5)
 * 
 * arguments: none
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
ACTION oig::endelection() {
  // authorize
  require_auth( get_self() );
  // initialize
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  // validate
  check(elect.state == 5, "Voting needs to have concluded.");
  elect.state = 6; // set cleanup state
  elections.set(elect, get_self()); // write changes
  state_refresh(); // start cleanup
}


//======================= utility methods ============================//

/*
 * syncvoter()  Called for every registered voter after the voting period ended before
 *              the ballot is closed. Syncronizes the vote balance of a user to his token stake.
 *              Rebalances the ballot according to the new stake.
 *              
 * requirements:
 *    (8,VOTE) treasury needs to exist.
 *    voter needs to exist and be registered as voter.
 *    ballot needs to exist and not be closed or archived.
 * 
 * arguments:
 *    name voter: account to be synchronized    name ballot:  ballot that is currently voted on
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void oig::syncvoter (name voter, name ballot){
  // set permission for inline actions
  permission_level permissionLevel = permission_level(get_self(), name("active"));
  action transferAction; 
  // prepare arguments for stake sync
  VoterArg sync;
  sync.voter = voter; // set arguments
  transferAction = action(
      permissionLevel,
      name("decide"), // contract to be called
      name("sync"), // action to be called
      std::move(sync));
  transferAction.send(); // call

  RebalArg args;
  args.voter = voter; // set arguments
  args.ballot = ballot;
  transferAction = action(
      permissionLevel,
      name("decide"), // contract to be called
      name("rebalance"), // action to be called
      std::move(args));
  transferAction.send(); // call
}

/*
 * state_refresh()  State refresh is the core of the OIG Election contract.
 *                  It guides the election based on the arguments provided during inauguration. 
 *                  It needs to be called recurringly to progress through the election states.
 *                  This is achieved by including it in user called actions and by manually
 *                  calling of updtstate(). For example by a cron job.
 * 
 *  states: 
 *    00  - contract clean: 
 *                The contract is prepared to start a new election process.
 *    01  - election created
 *                Election is created, once elect.nmn_open is reached the contrac progresses to state 2,
 *                allowing nominations.
 *    02  - nomination in progress
 *                Waiting for Nominations to close. Once the deadline is reached a vector containing accepted
 *                nominations is build and the ballot created and set up.
 *                This includes sending a 30 WAX fee to decide.
 *                Contract is moved into state 3 afterward.
 *    03  - nomination closed
 *                Once elect.vote_open is reached, decide will be called to open the ballot with the given end date.
 *                and move the contract into state 4.
 *    04  - voting in progress
 *                Contract will wait for elect.vote_close. 
 *                Once passed state_refresh() will start synchronizing all registered voters and 
 *                rebalancing the current ballot. To not run into excution time limits, 
 *                only 100 voters are synchronized per batch. As we could not test this yet, 
 *                this number might need to be reduced further.
 *                Once all users are synchronized the ballot is closed and the contract moved into state 5.
 *    05  - voting commenced
 *                Voting has come to an end, and a winner should have been determined.
 *                The contract remains in state 5 for data persistence until 
 *                endelection() is called manually.
 *    06  - cleanup initiated
 *                During cleanup all nominations and nominee info are deleted.
 *                Voters are reset to not synced by moving them back to the elect.voter vector.
 *  Requirements:
 *    To host a ballot get_self needs to hold 30 WAX.
 *    elect.ballot is used as primary key for the ballot and needs to be unique.
 *    To open the voting the ballot needs to contain at least 2 option to vote on.
 * 
 *  TODO:
 *    Change nominee tracking to a pre-built vector.
 *    Publish results / archive ballots.
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
uint8_t oig::state_refresh() {
    election_singleton elections(get_self(), get_self().value);
    auto elect = elections.get();

    auto now = time_point_sec(current_time_point());

    permission_level permissionLevel = permission_level(get_self(), name("active"));
    action transferAction;

    switch (elect.state) {
        case (0): // no current election
          return elect.state;

        case (1): // Election created, nominations not opened
            if (now >= elect.nmn_open) {
                elect.state = 2; // open nominations
                elections.set(elect, get_self());
            }
            break;

        case (2): // nominations open
            if (now >= elect.nmn_close) { // once deadline is passed proceed to next stage
              vector<name> ballot_options; // create the vector containing the candidates as ballot options

              // we have a possible attack vector here.
              // spammers could flood the nominee pool,
              // leading to this transaction not being executed timely.
              // To prevent this limitations to nominations have been put into place.
              // In the future this needs to be reworked to allow for batched execution in case of spam.
              // Placing a nomination fee is recommended.
              nominations_table nominations(get_self(), get_self().value);
              for (auto nmnt = nominations.begin(); nmnt != nominations.end(); ++nmnt) {
                if (nmnt->accepted) {
                  ballot_options.push_back(nmnt->nominee);
                }
              }
              // It needs to be ensured that at least two candidates accepted the nomination.
              // It is not possible to host an election with just one candidate.
              // A possible measure would be to just enlist the contract itself as dummy.
              if (size(ballot_options) >= 2 ){
                // pay the Ballot fee of currently 30 WAX
                BallotFeeArguments blargs;
                transferAction= action(
                    permissionLevel,
                    name("eosio.token"),
                    name("transfer"),
                    std::move(blargs)
                );
                transferAction.send();

                // create the ballot
                NewBallotArguments args;
                  args.ballot = elect.ballot; // ballot p-key
                  args.publisher = get_self();
                  args.options = ballot_options;        
                transferAction= action(
                    permissionLevel,
                    name("decide"),
                    name("newballot"),
                    std::move(args)
                );
                transferAction.send();

                // set ballot details
                BallotDetailArguments details;
                    details.ballot = elect.ballot;          //ballot name
                    details.title = elect.title;            //ballot title
                    details.description = elect.description;//ballot description
                    details.content = elect.content;        //ballot content
                transferAction = action(
                    permissionLevel,
                    name("decide"),
                    name("editdetails"),
                    std::move(details)
                );
                transferAction.send();

                // toggle voting mechanism
                ToggleArguments toggle; // ballots are initialized without a voting mechanism
                    toggle.ballot = elect.ballot; // as we want to only count staked tokens
                transferAction = action(          // we need to toggle votestake
                    permissionLevel,
                    name("decide"),
                    name("togglebal"),
                    std::move(toggle)
                );
                transferAction.send();
                
                elect.state = 3; // close nominations
                elections.set(elect, get_self()); // write changes to election state
              }
            }
            break;

        case (3): // nominations closed
            if (now >= elect.vote_open) {
                // open the voting
                OpenArguments open;
                    open.ballot = elect.ballot; 
                    open.end_time = elect.vote_close;
                transferAction = action(
                    permissionLevel,
                    name("decide"),
                    name("openvoting"),
                    std::move(open)
                );
                transferAction.send();

                elect.state = 4; // open voting
                elections.set(elect, get_self()); // write changes to election state
            }
            break;

        case (4): // voting open
            if (now >= elect.vote_close) {

              uint8_t i = 0;
              name voter;

              // synchronize and rebalance voters in batches of 100 to prevent
              // the transaction of exceeding time limits.
              while (elect.voter.size() != 0 && i < 100) {
                voter = elect.voter.back(); 
                syncvoter(voter, elect.ballot); //sync and rebalance
                elect.synced_voter.push_back(voter); // remember regged voters for future elections
                elect.voter.pop_back(); // remove synced voter
                i++;
              }
              if (elect.voter.empty()) { // only close voting once all votes are synced
                CloseArguments close;
                close.ballot = elect.ballot;
                transferAction = action(
                    permissionLevel,
                    name("decide"),
                    name("closevoting"),
                    std::move(close));
                transferAction.send();

                elect.state = 5; // close voting
              }
              elections.set(elect, get_self());
            }
            break;

        case (5): // voting concluded
            
            break;

        case (6): // election ended / cancelled
            cleanup();
            break;
        default:
            printf("Your princess is in another castle.");
    }
    return elect.state;
}

/*
 *  cleanup()  Clears nominations, nominee info and swaps votes back into the unsynced pool.
 *             As voter registration is always open it needs to be doublechecked if users registered since last sync.
 * 
 *  authorisation: contract
 *  requirements: Election in cleanup state.
 *  
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void oig::cleanup(){
  require_auth( get_self() );
  // clear nominee info table
  nominees_table nominees(get_self(), get_self().value);
  auto nmne = nominees.begin();
  while (nmne != nominees.end()) {
      nmne = nominees.erase(nmne);
  }
  // clear nominations table
  nominations_table nominations(get_self(), get_self().value);
  auto nomn = nominations.begin();
  while (nomn != nominations.end()) {
      nomn = nominations.erase(nomn);
  }

  // reset the nominations counter
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  elect.nom_count = 0;
  
  uint8_t i = 0;
  name voter;
  
  if(!elect.synced_voter.empty()) { // if voters got synced in the prior election they need to be moved
    if (!elect.voter.empty()) { // to not override entries we need to check if we have remaining entries
      while (!elect.voter.empty() && i < 200) { // this is executed in batches to prevent time exceeds
        voter = elect.voter.back();
        elect.synced_voter.push_back(voter); // and if so move them to the synced pool for preservation
        elect.voter.pop_back();
        i++;
      }
    } else {
      elect.voter = elect.synced_voter; // write all voters back to the pool.
      elect.synced_voter.clear();
      elect.state = 0;  // resetting the election state
    }
  }
  elections.set(elect, get_self());
}



//========== testing functions ==========

ACTION oig::setballot(name id) {
  require_auth( get_self() );
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  elect.ballot = id;
  elections.set(elect, get_self());
}

/*
 * function() 
 * 
 * authorisation: 
 * requirements:
 * 
 * arguments:
 * 
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::fakenom() {
  require_auth( get_self() );

  uint64_t tmp = name("fake").value;
  name nominee;

  nominations_table nominations(get_self(), get_self().value);
  uint8_t i;

  for (i = 0; i < 250; i++) {
    nominee = name(tmp);
    nominations.emplace(get_self(), [&](auto& col) {
      col.nominee = nominee;
      col.accepted = 0;
    });
    tmp++;
  }
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  elect.nom_count += i;
  elections.set(elect, get_self());
}



 *
 * simulate() proceeds an election to the next state by changing times.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::simulate(){
  require_auth( get_self() );

  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();

  auto now = time_point_sec(current_time_point());

  switch (elect.state) {
      case (1): // Election created, nominations not opened
          elect.nmn_open = now; // open nominations
          elections.set(elect, get_self());
          state_refresh();
          break;

      case (2): // nominations open
          elect.nmn_close = now;
          elections.set(elect, get_self());
          state_refresh();
          break;

      case (3): // nominations closed
          elect.vote_open = now;
          elect.vote_close = now + 900;
          elections.set(elect, get_self());
          state_refresh();
          break;

      case (4): // voting open
          state_refresh();
          break;

      case (5): // voting concluded
          state_refresh();
          break;

      default:
          printf("Your princess is in another castle.");
  }
}

 *
 * setvoters() allows to manually SET regged voters to be tracked.
 *             This will overwrite existing voters and should be used with caution.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::setvoters(vector<name> voters) {
  require_auth( get_self() );
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  elect.voter = voters;
  elections.set(elect, get_self());
}

 *
 * reset()  deletes the election singleton for table changes.
 *          Use with caution as a wrong init state or ballot key can deadlock the contract.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::reset() {
  require_auth( get_self() );
  election_singleton elections(get_self(), get_self().value);
  elections.remove();
}

 *
 * addnomn()  allows to add a nominee to an existing ballot prior to the election beginning, 
 *            Only to be used to fix deadlocks during testing.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::addnomn(name nominee){
  require_auth( get_self() );
  permission_level permissionLevel = permission_level(get_self(), name("active"));
  action transferAction;
  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();
  struct Option {
      name ballot;
      name option;
  };
  Option opt;
  opt.option = elect.ballot;
  opt.option = nominee;
  transferAction = action(
      permissionLevel,
      name("decide"),
      name("addoption"),
      std::move(opt)
  );
  transferAction.send();
}

 *
 * addvoter() Adds an already registerd voter to the contracts tracking.
 *            Only needed for accounts used during first testing period.
 *            Do not use unless you are SURE the voter is registered on Decide for (8,VOTE)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
ACTION oig::addvoter(name voter) {
  require_auth( get_self() );
  check(is_account(voter), "Voter account must exist.");

  election_singleton elections(get_self(), get_self().value);
  auto elect = elections.get();

  reggedvoters_table reggedvoters(get_self(), voter.value);
  auto voter_itr = reggedvoters.find(get_self().value);
  if (voter_itr == reggedvoters.end()) {
    reggedvoters.emplace(get_self(), [&](auto& col) {
      col.referrer = get_self();
      col.treasury = VOTE_SYM;
      col.voter = voter;
    });
    if(!elect.synced_voter.empty()) {
      if (!elect.voter.empty() ) {
        elect.voter.push_back(voter);
      } else {
        elect.synced_voter.push_back(voter);
      }
    } else {
      elect.voter.push_back(voter);
    }
    elections.set(elect, get_self());
  }
}*/