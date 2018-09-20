#!/usr/bin/env python

import rospy

from bitbots_stackmachine.stack_machine import StackMachine
from tiago_bartender_stackmachine.decisions import Init
from tiago_bartender_stackmachine.blackboard import Blackboard

class TiagoBartender:
    def __init__(self):
        rospy.init_node('tiago_bartender_stackmachine')

        self.blackboard = Blackboard()
        self.stackmachine = StackMachine(self.blackboard, "controller")
    	self.stackmachine.set_start_element(Init)

        #TODO self.pause_card_subscriber(..., self.pause_card_cb)
        #TODO self.redo_card_subscriber(..., self.redo_card_cb)

        self.run()        

    def run(self):
        rate = rospy.Rate(10)
        while not rospy.is_shutdown():
            self.stackmachine.update()

    def pause_card_cb(self, msg):
        self.blackboard.was_pause_card_shown = True

    def redo_card_cb(self, msg):
        self.blackboard.redo_requested = True


def main():
    bartender = TiagoBartender()
    bartender.run()


if __name__ == '__main__':
    main()
