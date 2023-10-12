#!/usr/bin/env python3
import argparse
import subprocess
import os
import re
import pathlib
from collections import OrderedDict

#If lustre adds or modifies changelog record types, these may have to be updated
MKDIR_CONSTANT = 2
RMDIR_CONSTANT = 7
RENAME_CONSTANT = 8

def main():

    parser = argparse.ArgumentParser(
                        prog='Lustre changelog parser',
                        description='Retrieves and parses a lustre changelog and converts it \
                        into a list of directories that need to be indexed. Last grabbed changelog \
                        record number will be prepended to file. ')

    parser.add_argument('-m', '--mdt', required=True, nargs='+',
                        help='Name of the lustre mdt\'s to retrieve changelogs from.')
    parser.add_argument('-p', '--path', required=True,
                        help='Absolute path to the lustre filesystem mount point, needed for lfs fid2path.')
    parser.add_argument('-o', '--output', required=True,
                        help='Output files that will be processed by gufi_changelog_update_index. \
                                Four files will be created (move, create, delete, update) that will \
                                be required by gufi_changelog_update_index.')
    parser.add_argument('-s', '--startrec', default='-', nargs='*',
                        help='Start record number of changelogs to get.')
    parser.add_argument('-e', '--endrec', default='-', nargs='*',
                        help='End record number of changelogs to get. ')

    args = parser.parse_args()

    if (len(args.mdt) != len(args.startrec)):
        print("Must have same number of mdt's as start record numbers")
        exit(1)


    lustre_changelog_list = []
    last_changelog_num_list = []
    #get lustre changelog
    try:
        for i in range(len(args.mdt)):
            lfs_changelog_output = subprocess.run(["sudo", "lfs", "changelog", args.mdt[i], args.startrec[i], args.endrec[i]], 
                                                stdout=subprocess.PIPE, check=True)

            #ignore argument may cause issues
            lfs_changelog_output = lfs_changelog_output.stdout.decode('utf-8', 'ignore').rstrip().split('\n')

            lfs_changelog_output_len = len(lfs_changelog_output)
            if lfs_changelog_output_len == 1:
                continue


            last_changelog = lfs_changelog_output[lfs_changelog_output_len - 1]
            last_changelog_num = last_changelog.split()[0]
            last_changelog_num_list.append(last_changelog_num)

            lustre_changelog_list.extend(lfs_changelog_output)

        #lustre_changelog_output = subprocess.run(["sudo", "lfs", "changelog", args.mdt, args.startrec, args.endrec], 
        #                                    stdout=subprocess.PIPE, check=True)
    except Exception as e:
        print("Retrieving lustre changelog failed.")
        print(e)
        exit(1)

    move_file = open(args.output + ".move", "w")
    create_file = open(args.output + ".create", "w")
    delete_file = open(args.output + ".delete" , "w")
    update_file = open(args.output + ".update", "w")

    recently_created_dir = {}

    move_dict = {}
    create_dict = {}
    delete_dict = {}
    update_dict = {}

    #prepend last changelog number grabbed to file
    for last_changelog_num in last_changelog_num_list:
        move_file.write(f'{last_changelog_num}\n')
        create_file.write(f'{last_changelog_num}\n')
        delete_file.write(f'{last_changelog_num}\n')
        update_file.write(f'{last_changelog_num}\n')

    #TODO refactor code to pull out regex search into separate method
    #TODO refactor code to pull out lfs fid2path call into separate method
    #TODO could potentially batch out lfs fid2path, should reduce RPC's being 
    #sent out
    for line in lustre_changelog_list:
        #get rec number type to compare
        changelog_fields = line.split()
        changelog_field_num = len(changelog_fields)
        changelog_rec_type = changelog_fields[1]
        rec_type = int(changelog_rec_type[slice(0,2)])

        #get parent of deleted directory, and append name of deleted directory to 
        #generate path of directory that has to be deleted. we will recursively delete everything under
        #this directory as it has already been deleted in the filesystem.
        if (rec_type == RMDIR_CONSTANT):
            target_fid = re.search(r"t=\[(.*?)\]", line).groups(0)[0]

            #directory has been created and deleted in same changelog slice
            if target_fid in recently_created_dir:
                del recently_created_dir[target_fid]
                continue

            parent_fid = re.search(r"p=\[(.*?)\]", line).groups(0)[0]

            lfs_fid2path_output = subprocess.run(["sudo","lfs","fid2path", args.path, parent_fid], 
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                encoding='utf-8')
            #parent may have already been deleted, that is fine, we will eventually find changeline 
            #record where parent has not been deleted. 
            if (lfs_fid2path_output.returncode != 0):
                continue

            directory_name = changelog_fields[changelog_field_num - 1]
            directory_path = os.path.join(lfs_fid2path_output.stdout.rstrip(), directory_name)

            #delete_file.write(f'{directory_path}\n')
            delete_dict[pathlib.Path(directory_path)] = 1
            continue

        #get the path of directory to tell GUFI what to update
        if (rec_type == MKDIR_CONSTANT):
            target_fid = re.search(r"t=\[(.*?)\]", line).groups(0)[0]

            recently_created_dir[target_fid] = 1

            lfs_fid2path_output = subprocess.run(["sudo","lfs","fid2path", args.path, target_fid], 
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                encoding='utf-8')
            
            #parent may have been deleted, this is fine as it will be caught by RMDIR
            #changelog
            if (lfs_fid2path_output.returncode != 0):
                continue

            #directory_path = os.path.join(lfs_fid2path_output.stdout.rstrip(), 
            #                                changelog_fields[changelog_field_num - 1])

            #create_file.write(f'{lfs_fid2path_output.stdout.rstrip()}\n')
            #update_file.write(f'{lfs_fid2path_output.stdout.rstrip()}\n')
            update_dict[pathlib.Path(lfs_fid2path_output.stdout.rstrip())] = 1
            create_dict[pathlib.Path(lfs_fid2path_output.stdout.rstrip())] = 1
            continue

        #two cases here, first is we are moving a file, in this case we get parents of both
        #source and destination
        #TODO potentially might have issues since we always record renames, which could result in 
        #duplicate pfids in file
        #second case, we are moving directories, in this case we have to tell GUFI where directory
        #needs to get moved to. 
        if (rec_type == RENAME_CONSTANT):
            source_fid = re.search(r"s=\[(.*?)\]", line).groups(0)[0]

            lfs_fid2path_output = subprocess.run(["sudo","lfs","fid2path", args.path, source_fid], 
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                encoding='utf-8')

            if (lfs_fid2path_output.returncode == 0):
                #check if fid being moved is a file or directory
                rename_source_path = lfs_fid2path_output.stdout.rstrip()
                is_file = os.path.isfile(rename_source_path)
            #fid has been deleted, assume its a directory
            else:
                is_file = False

            if (is_file):
                source_parent_fid = re.search(r"sp=\[(.*?)\]", line).groups(0)[0]
                parent_fid = re.search(r" p=\[(.*?)\]", line).groups(0)[0]

                lfs_fid2path_output_source = subprocess.run(["sudo","lfs","fid2path", args.path, source_parent_fid],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, 
                                    encoding='utf-8')

                lfs_fid2path_output_parent = subprocess.run(["sudo","lfs","fid2path", args.path, parent_fid], 
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, 
                                    encoding='utf-8')

                if (lfs_fid2path_output_source.returncode == 0):
                    #update_file.write(f'{lfs_fid2path_output_source.stdout.rstrip()}\n')
                    update_dict[pathlib.Path(lfs_fid2path_output_source.stdout.rstrip())] = 1

                if (lfs_fid2path_output_parent.returncode == 0):
                    #update_file.write(f'{lfs_fid2path_output_parent.stdout.rstrip()}\n')
                    update_dict[pathlib.Path(lfs_fid2path_output_parent.stdout.rstrip())] = 1

            #fid is a directory
            else:
                #getting both sp and p fids, along with name of directory being moved
                source_parent_regex = re.search(r"sp=\[(.*?)\]( .*)", line).groups(0)
                source_regex = re.search(r"s=\[(.*?)\]", line).groups(0)
                parent_regex = re.search(r" p=\[(.*?)\](.\S*)", line).groups(0)

                lfs_fid2path_output_source_parent = subprocess.run(["sudo","lfs","fid2path", args.path, source_parent_regex[0]], 
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, 
                                    encoding='utf-8')

                lfs_fid2path_output_parent = subprocess.run(["sudo","lfs","fid2path", args.path, parent_regex[0]], 
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, 
                                    encoding='utf-8')

                if (lfs_fid2path_output_source_parent.returncode == 0):
                    source_parent_path = lfs_fid2path_output_source_parent.stdout.rstrip()
                    move_dict[pathlib.Path(source_parent_path)] = 1
                    #move_file.write(f'{source_parent_path}\n')

                if (lfs_fid2path_output_parent.returncode == 0):
                    parent_path = lfs_fid2path_output_parent.stdout.rstrip()
                    move_dict[pathlib.Path(parent_path)] = 1
                    #move_file.write(f'{parent_path}\n')
            continue

        #rest of changelog record types, get parent directory to tell GUFI what to update
        parent_fid = re.search(r"p=\[(.*?)\]", line).groups(0)[0]
        lfs_fid2path_output = subprocess.run(["sudo","lfs","fid2path", args.path, parent_fid], 
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            encoding='utf-8')

        if (lfs_fid2path_output.returncode != 0):
            continue

        #update_file.write(f'{lfs_fid2path_output.stdout.rstrip()}\n')
        update_dict[pathlib.Path(lfs_fid2path_output.stdout.rstrip())] = 1

    #move_list = list(OrderedDict.fromkeys(move_list))
    
    #move_list.sort()

    for path in move_dict.copy():
        for parents in path.parents:
            if parents in move_dict:
                del move_dict[path]
                break

    for path in create_dict.copy():
        for parents in path.parents:
            if parents in move_dict:
                del create_dict[path]
                break

    for path in delete_dict.copy():
        for parents in path.parents:
            if parents in move_dict:
                del delete_dict[path]
                break

    for path in update_dict.copy():
        for parents in path.parents:
            if parents in move_dict:
                del update_dict[path]
                break

    for path in move_dict:
        move_file.write(f'{path}\n')

    for path in create_dict:
        create_file.write(f'{path}\n')

    for path in delete_dict:
        delete_file.write(f'{path}\n')

    for path in update_dict:
        update_file.write(f'{path}\n')

    move_file.close()
    create_file.close()
    delete_file.close()
    update_file.close()

if __name__ == '__main__':
    main()
