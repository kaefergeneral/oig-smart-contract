#include <eosio/eosio.hpp>
#include <eosio/singleton.hpp>
#include <eosio/action.hpp>
#include <eosio/asset.hpp>

using namespace std;
using namespace eosio;

namespace oigspace {

    CONTRACT oig : public contract {
        public:
            using contract::contract;
            static constexpr symbol WAX_SYM = symbol("WAX", 8);     // The system token and it's decimal places
            static constexpr symbol VOTE_SYM = symbol("VOTE", 8);   // The treasury symbol referring to WAX_SYM

            /*
             *   Registers the contract as voter on decide to allow it to create ballots.
             *   Sets the contract state to clean.
             *   Requires decide to be intitialized and treasury to be set up.
             *   auth: oig
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION init();

            /*
             *   Initializes or cancels the election process
             *   auth: oig
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION inaugurate( string title, string description, string content, time_point_sec nmn_open, time_point_sec nmn_close, time_point_sec vote_open, time_point_sec vote_close, bool cancel);
            
            /*
             *   Allows one account to nominate itself or someone else.
             *   Self nominations are automatically accepted.
             *   auth: nominator
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION nominate(name nominator, name nominee);

            /*
             *   Called to accept or decline a nomination.
             *   Declining a nomintation will delete it.
             *   auth: nominee
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION proclaim(name nominee, bool decision);

            /*
             *   Allows nominees to provide personal info or delete given info.
             *   auth: nominee
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION nominf(name nominee, string name, string descriptor, string picture, string telegram, string twitter, string wechat, bool remove);

            /*
             *   Calls refresh_state() to progress in election if needed
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION updtstate();

            /*
             *   Verifies an account is registered as voter with decide in the (8,Vote) treasury.
             *   Registers and adds it to the tracking pool if need.
             *   auth: voter
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION regvoter(name voter);

            /*
             *   Sets an ended election into cleanup state and starts the cleanup process.
             *   auth: oig
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION endelection();


            /*
             *   Cleanup function.
             *   auth: oig
             *   TODO: move to private after data election.
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            ACTION cleanup();


            /*
             *   Debug and maintenance functions. To be removed for go live.
             *   Consult source documentation before execution.
             *   auth: oig
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * 
            ACTION addnomn(name nominee);
            ACTION simulate();
            ACTION setvoters(vector<name> voters);
            ACTION fakenom();
            ACTION reset();
            ACTION addvoter(name voter);*/
            ACTION setballot(name id);
            

        private:

            /*
             *   Synchronizes a users vote stake with his WAX stake during an election.
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            void syncvoter(name voter, name ballot);

            /*
             *   Election contract core logic. Progresses the election through it's stages.
             *   Consult source documentation for further clarification.
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            uint8_t state_refresh();



            //==================================== tables ====================================

            // elections table
            // scope: self
            TABLE election { 
                name ballot = name("oig"); // stores the primary key for an election to be used in ballot creation
                uint8_t state = 0;  // tracks the contract state
                string title;       // Election title
                string description; // Election description
                string content;     // IPFS link or URL for further details
                uint8_t nom_count = 0;  // Tracking the nomination count
                vector<name> voter; // Containing all voters registered with (8,VOTE) that need to be synchronized
                vector<name> synced_voter; // Stores synchronized voters during ballot evalutation
                time_point_sec nmn_open;   //time that nominations begin
                time_point_sec nmn_close;  //time that nominations close
                time_point_sec vote_open;  //time that voting can be opened
                time_point_sec vote_close; //time that voting closes
                EOSLIB_SERIALIZE(election, (ballot)(state)(title)(description)(content)(nom_count)(voter)(synced_voter)(nmn_open)(nmn_close)(vote_open)(vote_close))
            };
            typedef singleton<name("election"), election> election_singleton;

            // voter table
            // scope: voter
            TABLE reggedvoter{
                name referrer; // tracks the owner of said treasury 
                symbol treasury; // treasury identifier
                name voter;
                auto primary_key() const { return referrer.value; } // possible conflict if we extend the scope of this contract
                EOSLIB_SERIALIZE(reggedvoter, (referrer)(treasury)(voter))
            };
            typedef multi_index<name("reggedvoters"), reggedvoter> reggedvoters_table;

            // nominations table
            // scope: self
            TABLE nomination {
                name nominee;
                bool accepted;
                uint64_t primary_key() const { return nominee.value; }
                EOSLIB_SERIALIZE(nomination, (nominee)(accepted))
            };
            typedef multi_index<name("nominations"), nomination> nominations_table;

            // nominees table
            // scope: self
            TABLE nominee {
                name owner;
                string name;        // max length   99 chars
                string descriptor;  // max length 2000 chars
                string picture;     // max length  256 chars (Needs to be an URL)
                string telegram;    // max length   99 chars
                string twitter;     // max length   99 chars
                string wechat;      // max length   99 chars
                auto primary_key() const { return owner.value; }
                EOSLIB_SERIALIZE(nominee, (owner)(name)(descriptor)(picture)(telegram)(twitter)(wechat))
            };
            typedef multi_index<name("nominees"), nominee> nominees_table;
            





            //==================================== structs ====================================
            /*
             *   Partly prefilled structs for inline action execution
             * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
            struct BallotFeeArguments { // eosio.token::transfer
                name sender = name("oig");
                name reciever = name("decide");
                asset quanitity = asset(3000000000, WAX_SYM); // ballot fee payment
                string memo = string("Ballot Fee Payment");
            };

            struct NewBallotArguments { //decide::newballot
                name ballot;
                name category = name("election");
                name publisher;
                symbol treasury = VOTE_SYM; // (8,VOTE)
                name method = name("1token1vote");
                vector<name> options;
            };

            struct BallotDetailArguments { //decide::editdetails
                name ballot;
                string title;
                string description;
                string content;
            };

            struct ToggleArguments { // decide::togglebal
                name ballot;
                name toggle = name("votestake"); // Count only staked tokens
            };
            
            struct OpenArguments {  // decide::openvoting
                name ballot;        // the ballot to be opened
                time_point_sec end_time; // other than the name suggests does not contain an open time
            };

            struct CloseArguments { // decide::closevoting
                name ballot;        // ballot to close
                bool broadcast = false; // allows to broadcast the results to be caught and handled
            };

            struct RegVoter { //decide::regvoter
                name voter; 
                symbol treasury_symbol = VOTE_SYM; // fixed (8,VOTE)
                name referrer = name("oig"); // needed as (8,VOTE) is private to enforce voter logging
            };

            struct VoterArg { //decide::sync
                name voter;
            };

            struct RebalArg { //decide:rebalance
                name voter;
                name ballot;
                name worker = name("oig");
            };
            
        };
}
